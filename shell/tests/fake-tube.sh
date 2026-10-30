#!/bin/sh
# Stand-in for runtime/vortex-tube: answers from tests/fixtures/tube.
F=$(cd "$(dirname "$0")" && pwd)/fixtures/tube
echo "fake-tube: $*" >&2
case $1 in
search) sleep 0.3; if [ "${4:-1}" = 1 ]; then cp "$F/search.tsv" "$3"; else cp "$F/page2.tsv" "$3"; fi ;;
popular | trending) cp "$F/search.tsv" "$2" ;;
feed) echo '!No subscriptions yet' > "$2"; exit 1 ;;
channel) cp "$F/search.tsv" "$3" ;;
video) cp "$F/video.tsv" "$3" ;;
instances) printf 'https://good.example\t1\t240\nhttps://bad.example\t0\t10000\n' > "$2" ;;
thumbs)
	mkdir -p "$VORTEX_TUBE_CACHE/thumbs/$2"
	shift 2
	for id; do cp "$F/thumb.jpg" "$VORTEX_TUBE_CACHE/thumbs/default/$id.jpg"; done ;;
play) sleep 30 ;;
esac
