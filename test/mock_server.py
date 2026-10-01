#!/usr/bin/env python3
"""A stand-in for Jev and OpenAI-compatible servers, so tests cost nothing.

Paths:  /jev/v1/systemone              answers like Jev
        /openai/v1/chat/completions    answers like an OpenAI-compatible server with logprobs
        /<fault>/...                   fails first: fail500, fail429, fail401, fail400, garbage, nologprobs, slow (2 s)
        /count  /reset                 requests seen since the last reset

Answers follow simple rules so tests can predict them: yes/no is 0.9 when the text says "broken", else 0.1;
pick chooses the first option named in the text, else the first option; score is the number of "!" (capped).

Run: python3 test/mock_server.py [port]   (default 8765)
"""

import json
import math
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

lock = threading.Lock()
count = 0


def judge(text, kind, options):
    text = text.lower()
    if kind == "yes_no":
        return {"yes": 0.9 if "broken" in text else 0.1}
    if kind == "pick":
        chosen = next((o for o in options if o.lower() in text), options[0])
        return {o: (0.8 if o == chosen else 0.2 / max(len(options) - 1, 1)) for o in options}
    level = min(text.count("!"), len(options) - 1)
    return {str(i): (1.0 if i == level else 0.0) for i in range(len(options))}


def jev_answer(body):
    state, answers = body["state"], {}
    for qid, q in body["questions"].items():
        instructions = q["instructions"]
        # Judge the row only, never the question, so answers depend on the data.
        text = (
            json.dumps(state[instructions["row"].strip("`")]) if isinstance(instructions, dict) else json.dumps(state)
        )
        if q["type"] == "noul":
            answers[qid] = {"type": "noul", "noul": judge(text, "yes_no", [])["yes"]}
        elif q["type"] == "choice":
            probs = judge(text, "pick", list(q["criteria"].keys()))
            answers[qid] = {
                "type": "choice",
                "choice": max(probs, key=probs.get),
                "probabilities": probs,
                "confidence": 0.7,
            }
        else:
            probs = judge(text, "score", q["criteria"])
            score = sum(int(k) * p for k, p in probs.items())
            answers[qid] = {"type": "score", "score": score, "probabilities": probs, "confidence": 0.9}
    return {"model": "jev-mock-1.0", "answers": answers, "usage": {"input_tokens": 1000, "output_tokens": 10}}


def openai_answer(body, with_logprobs=True):
    prompt = body["messages"][-1]["content"]
    lines = prompt.split("\n")
    options = [l.split(") ", 1)[1].split(" (")[0] for l in lines if len(l) > 2 and l[1] == ")"]
    labels = [l[0] for l in lines if len(l) > 2 and l[1] == ")"]
    kind = "yes_no" if "Answer Y for yes" in prompt else ("pick" if "letter" in prompt else "score")
    probs = judge(prompt.split("\n\nQuestion:")[0], kind, options)
    if kind == "yes_no":
        top = [("Y", probs["yes"]), ("N", 1 - probs["yes"])]
    else:
        top = [(labels[i], p) for i, p in enumerate(probs.values())]
    top = [{"token": t, "logprob": math.log(max(p, 1e-9))} for t, p in top]
    choice = {"index": 0, "message": {"role": "assistant", "content": top[0]["token"]}}
    if with_logprobs:
        choice["logprobs"] = {
            "content": [{"token": top[0]["token"], "logprob": top[0]["logprob"], "top_logprobs": top}]
        }
    return {"model": "mock-small", "choices": [choice], "usage": {"prompt_tokens": 500, "completion_tokens": 1}}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, payload, headers=None):
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def get_body(self):
        global count
        with lock:
            if self.path.startswith("/reset"):
                count = 0
                return b"ok"
            return str(count).encode()

    def do_GET(self):
        self.reply(200, self.get_body())

    def do_HEAD(self):
        # read_text() asks for the size first. HEAD on /reset must not reset, so it reports "ok".
        with lock:
            size = 2 if self.path.startswith("/reset") else len(str(count))
        self.send_response(200)
        self.send_header("Content-Length", str(size))
        self.end_headers()

    def do_POST(self):
        global count
        with lock:
            count += 1
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        fault = self.path.split("/")[1]
        if fault == "fail500":
            return self.reply(500, {"error": "boom"})
        if fault == "fail429":
            return self.reply(429, {"error": "slow down"}, {"Retry-After": "0"})
        if fault == "fail401":
            return self.reply(401, {"error": "bad key"})
        if fault == "fail400":
            return self.reply(400, {"error": "bad request"})
        if fault == "slow":
            time.sleep(2)
        if fault == "garbage":
            return self.reply(200, b"not json")
        if "chat/completions" in self.path:
            return self.reply(200, openai_answer(body, with_logprobs=fault != "nologprobs"))
        return self.reply(200, jev_answer(body))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
