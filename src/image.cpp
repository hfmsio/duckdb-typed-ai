#include "typed_ai/image.hpp"

#include "duckdb/common/types/hash.hpp"

namespace duckdb {
namespace typed_ai {

namespace {

// Jev's limits for one image: https://docs.typesafe.ai/images.md#limits
constexpr idx_t MIN_SIDE = 280;
constexpr idx_t MAX_SIDE = 1568;
constexpr idx_t MAX_FILE_BYTES = 16 << 20;
// A request is at most 32 MB, and a photo grows by a third when sent as text; leave room for the rest.
constexpr idx_t MAX_REQUEST_PHOTO_BYTES = 23 << 20;

idx_t Big16(const uint8_t *p) {
	return idx_t(p[0]) << 8 | p[1];
}

idx_t Big32(const uint8_t *p) {
	return Big16(p) << 16 | Big16(p + 2);
}

//! A JPEG's size sits in its first frame header (SOF), after any number of other segments.
void ReadJpegSize(const uint8_t *b, idx_t n, Image &image) {
	idx_t i = 2;
	while (i + 9 < n && b[i] == 0xFF) {
		auto marker = b[i + 1];
		if (marker == 0xFF) {
			i++; // fill byte
			continue;
		}
		bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
		if (frame) {
			image.height = Big16(b + i + 5);
			image.width = Big16(b + i + 7);
			return;
		}
		i += 2 + Big16(b + i + 2);
	}
}

} // namespace

Image Image::Of(const Value &value) {
	Image image;
	if (value.type().id() != LogicalTypeId::BLOB || value.IsNull()) {
		return image;
	}
	auto &bytes = StringValue::Get(value);
	auto b = const_data_ptr_cast(bytes.data());
	auto n = bytes.size();
	auto starts = [&](idx_t at, const char *text) {
		auto len = strlen(text);
		return n >= at + len && memcmp(b + at, text, len) == 0;
	};
	if (starts(0, "\x89PNG\r\n\x1a\n") && n >= 24) {
		image.format = "png";
		image.width = Big32(b + 16);
		image.height = Big32(b + 20);
	} else if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
		image.format = "jpg";
		ReadJpegSize(b, n, image);
	} else if (starts(0, "GIF8")) {
		image.format = "gif";
	} else if (starts(0, "RIFF") && starts(8, "WEBP")) {
		image.format = "webp";
	} else if (starts(4, "ftypheic") || starts(4, "ftypheix") || starts(4, "ftypmif1")) {
		image.format = "heic";
	}
	return image;
}

string Image::Problem(idx_t file_bytes) const {
	if (format != "png" && format != "jpg") {
		return StringUtil::Format("this photo is %s; only PNG and JPEG photos work. Convert it first",
		                          StringUtil::Upper(format));
	}
	if (width == 0 || height == 0) {
		return "this photo's size could not be read; the file may be damaged. Save it again as PNG or JPEG";
	}
	if (file_bytes > MAX_FILE_BYTES) {
		return StringUtil::Format("this photo's file is %d MB; the most is 16 MB. Save it smaller first",
		                          file_bytes >> 20);
	}
	if (width < MIN_SIDE || height < MIN_SIDE || width > MAX_SIDE || height > MAX_SIDE) {
		return StringUtil::Format("this photo is %d x %d pixels; each side must be %d to %d. Resize it first, e.g. "
		                          "magick mogrify -resize '1568x1568>' *.jpg",
		                          width, height, MIN_SIDE, MAX_SIDE);
	}
	return "";
}

bool Photo::Of(const Value &value, Photo &photo) {
	photo.image = Image::Of(value);
	if (!photo.image.IsPhoto()) {
		return false;
	}
	auto &bytes = StringValue::Get(value);
	photo.bytes = make_shared_ptr<const string>(bytes);
	auto hash = uint64_t(Hash(bytes.data(), bytes.size()));
	static const char *hex = "0123456789abcdef";
	photo.ref = "photo-";
	for (int shift = 60; shift >= 0; shift -= 4) {
		photo.ref += hex[(hash >> shift) & 0xF];
	}
	return true;
}

void AddPhoto(vector<Photo> &photos, const Photo &photo) {
	for (auto &existing : photos) {
		if (existing.ref == photo.ref) {
			return;
		}
	}
	photos.push_back(photo);
}

string PhotosProblem(const vector<Photo> &row, const vector<Photo> &options) {
	idx_t total = 0;
	for (auto list : {&row, &options}) {
		for (auto &photo : *list) {
			auto why = photo.image.Problem(photo.bytes->size());
			if (!why.empty()) {
				return why;
			}
			total += photo.bytes->size();
		}
	}
	if (total > MAX_REQUEST_PHOTO_BYTES) {
		return StringUtil::Format("this row's photos add up to %.1f MB; one request takes at most 23 MB. Use fewer "
		                          "or smaller photos",
		                          double(total) / (1 << 20));
	}
	return "";
}

} // namespace typed_ai
} // namespace duckdb
