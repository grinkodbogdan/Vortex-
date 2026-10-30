#!/usr/bin/python3
# vortex-tube against a stand-in Invidious server on 127.0.0.1, with a fake
# mpv that records its arguments.  Run from runtime/tests/run.sh.
import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
TUBE = os.path.join(HERE, "..", "vortex-tube")

VIDEOS = [{"type": "video", "videoId": "abc123def45", "title": "First\tvideo", "author": "Someone",
           "authorId": "UCsomeone", "lengthSeconds": 125, "viewCount": 4321, "publishedText": "1 day ago",
           "published": 200},
          {"type": "video", "videoId": "live0000000", "title": "Live now", "liveNow": True},
          {"type": "channel", "author": "A channel"},
          {"type": "video", "videoId": "zzz999yyy88", "title": "Second", "author": "Other",
           "authorId": "UCother", "lengthSeconds": 3700, "viewCount": 12, "publishedText": "2 years ago",
           "published": 100}]
DETAIL = {"videoId": "abc123def45", "title": "First video", "author": "Someone", "authorId": "UCsomeone",
          "lengthSeconds": 125, "viewCount": 4321, "likeCount": 99, "publishedText": "1 day ago",
          "description": "Line one\nLine\ttwo \\ end", "recommendedVideos": VIDEOS[3:],
          "adaptiveFormats": [
              {"itag": "140", "type": "audio/mp4; codecs=\"mp4a.40.2\"", "bitrate": "130000",
               "url": "/videoplayback?itag=140&id=abc"},
              {"itag": "251", "type": "audio/webm; codecs=\"opus\"", "bitrate": "150000",
               "url": "/videoplayback?itag=251&id=abc"},
              {"itag": "160", "type": "video/mp4; codecs=\"avc1.4d400c\"", "resolution": "144p",
               "url": "/videoplayback?itag=160&id=abc"},
              {"itag": "133", "type": "video/mp4; codecs=\"avc1.4d4015\"", "resolution": "240p",
               "url": "/videoplayback?itag=133&id=abc"},
              {"itag": "242", "type": "video/webm; codecs=\"vp9\"", "resolution": "240p",
               "url": "/videoplayback?itag=242&id=abc"},
              {"itag": "394", "type": "video/mp4; codecs=\"av01.0.00M.08\"", "resolution": "144p",
               "url": "/videoplayback?itag=394&id=abc"},
              {"itag": "134", "type": "video/mp4; codecs=\"avc1.4d401e\"", "resolution": "360p",
               "url": "/videoplayback?itag=134&id=abc"}],
          "formatStreams": [{"itag": "18", "url": "/latest_version?id=abc&itag=18&local=true"}]}
JPEG = b"\xff\xd8\xff\xe0fakejpeg"


class Handler(BaseHTTPRequestHandler):
    broken = False

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.end_headers()
        self.wfile.write(body if isinstance(body, bytes) else json.dumps(body).encode())

    def do_GET(self):
        p = self.path
        if Handler.broken:
            return self.send(403, b"forbidden", "text/plain")
        if p.startswith("/api/v1/search"):
            self.send(200, VIDEOS if "page=1" in p else [])
        elif p.startswith("/api/v1/trending") or p.startswith("/api/v1/popular"):
            self.send(200, VIDEOS)
        elif p.startswith("/api/v1/channels/"):
            self.send(200, {"videos": VIDEOS})
        elif p.startswith("/api/v1/videos/abc123def45"):
            self.send(200, DETAIL)
        elif p.startswith("/vi/"):
            self.send(200, JPEG, "image/jpeg")
        else:
            self.send(404, {"error": "no"})


def main():
    srv = HTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % srv.server_port
    t = tempfile.mkdtemp()
    os.makedirs(t + "/bin")
    with open(t + "/bin/mpv", "w") as f:
        f.write("#!/bin/sh\nfor a; do echo \"$a\"; done > \"$MPV_ARGS\"\n")
    os.chmod(t + "/bin/mpv", 0o755)
    os.makedirs(t + "/dir")
    with open(t + "/dir/settings", "w") as f:
        f.write("instance=%s\nquality=240\n" % base)
    env = dict(os.environ, VORTEX_TUBE_DIR=t + "/dir", VORTEX_TUBE_CACHE=t + "/cache",
               PATH=t + "/bin:" + os.environ["PATH"], MPV_ARGS=t + "/mpv-args",
               VORTEX_TUBE_INSTANCES=base + "/instances.json")
    fails = 0

    def run(*args):
        return subprocess.run([sys.executable, TUBE] + list(args), env=env, capture_output=True, text=True,
                              timeout=60)

    def check(name, ok):
        nonlocal fails
        print(("ok   " if ok else "FAIL ") + name)
        fails += not ok

    def lines(path):
        with open(path) as f:
            return f.read().splitlines()

    r = run("search", "lo fi", t + "/s.tsv")
    out = lines(t + "/s.tsv") if r.returncode == 0 else []
    check("tube: search lists videos only, tabs flattened", r.returncode == 0 and len(out) == 2 and
          out[0].split("\t") == ["abc123def45", "First video", "Someone", "UCsomeone", "125", "4321", "1 day ago"])
    r = run("search", "lo fi", t + "/s2.tsv", "2")
    check("tube: empty page", r.returncode == 0 and lines(t + "/s2.tsv") == [])

    r = run("video", "abc123def45", t + "/v.tsv")
    out = lines(t + "/v.tsv") if r.returncode == 0 else []
    head = out[0].split("\t") if out else []
    check("tube: video page", len(head) == 10 and head[0] == "V" and head[7] == "99" and
          head[9] == "Line one\\nLine\\ttwo \\\\ end" and len(out) == 2 and out[1].startswith("zzz999yyy88\t"))

    r = run("thumbs", "default", "abc123def45", "../etc")
    check("tube: thumbnails through the server", r.returncode == 0 and
          open(t + "/cache/thumbs/default/abc123def45.jpg", "rb").read() == JPEG and
          os.listdir(t + "/cache/thumbs/default") == ["abc123def45.jpg"])

    run("play", "abc123def45", "video", t + "/sock", "30")
    args = lines(t + "/mpv-args")
    check("tube: 240p H.264 + m4a audio, from 30 s", "--vo=drm" in args and "--start=30" in args and
          args[-1] == base + "/videoplayback?itag=133&id=abc" and
          "--audio-file=" + base + "/videoplayback?itag=140&id=abc" in args and
          "--input-ipc-server=" + t + "/sock" in args)
    with open(t + "/dir/settings", "w") as f:
        f.write("instance=%s\nquality=144\n" % base)
    run("play", "abc123def45", "video", t + "/sock")
    check("tube: 144p picks itag 160, not AV1", lines(t + "/mpv-args")[-1].endswith("itag=160&id=abc"))
    run("play", "abc123def45", "audio", t + "/sock")
    args = lines(t + "/mpv-args")
    check("tube: listen = audio only", "--no-video" in args and args[-1].endswith("itag=140&id=abc"))

    r = run("play", "../../x", "audio", t + "/sock")
    check("tube: bad id refused", r.returncode != 0)

    Handler.broken = True
    r = run("popular", t + "/p.tsv")
    check("tube: server error -> '!message' and exit 1", r.returncode == 1 and
          lines(t + "/p.tsv")[0].startswith("!"))
    srv.shutdown()
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
