#pragma once

#include "duckdb.hpp"

namespace duckdb {
namespace typed_ai {

//! A photo given as a row's input: a BLOB holding a PNG or JPEG file, e.g. `content` from read_blob('*.jpg').
struct Image {
	//! "png" or "jpg" for a photo Jev reads; "gif", "webp" or "heic" for one it does not; empty for any other value.
	string format;
	idx_t width = 0;
	idx_t height = 0;

	//! Reads the format and size from the file's first bytes. Nothing is decoded.
	static Image Of(const Value &value);
	bool IsPhoto() const {
		return !format.empty();
	}
	//! Empty when Jev will take it; otherwise why not and how to fix it. Jev refuses, never resizes.
	string Problem(idx_t file_bytes) const;
	//! Jev bills about one input token per 380 pixels (measured October 2026); a little more here, to be safe.
	idx_t Tokens() const {
		return width * height / 350 + 1;
	}
};

//! A photo sent with a request. The request names it by `ref`, and Jev swaps that name for the picture.
struct Photo {
	//! "photo-" and a hash of the bytes, so the same file always gets the same name.
	string ref;
	Image image;
	shared_ptr<const string> bytes;

	//! False when `value` is not a photo (any format Image::Of knows).
	static bool Of(const Value &value, Photo &photo);
};

//! Adds `photo` unless one with the same ref is already there.
void AddPhoto(vector<Photo> &photos, const Photo &photo);

//! Empty when Jev will take all of a row's photos (its own and its options') in one request; otherwise why not.
string PhotosProblem(const vector<Photo> &row, const vector<Photo> &options);

} // namespace typed_ai
} // namespace duckdb
