#!/bin/sh

set -eu

LC_ALL=C
export LC_ALL

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
AVCUT=${AVCUT:-"$ROOT/avcut"}
FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/avcut-tests.XXXXXX")

cleanup() {
	rm -rf "$WORKDIR"
}
trap cleanup EXIT HUP INT TERM

fail() {
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

run_avcut() {
	name=$1
	shift
	if ! "$AVCUT" "$@" >"$WORKDIR/$name.log" 2>&1; then
		cat "$WORKDIR/$name.log" >&2
		fail "$name: avcut failed"
	fi
}

assert_clean_decode() {
	name=$1
	file=$2
	if ! "$FFMPEG" -hide_banner -loglevel error -i "$file" -map 0 -f null - \
			>/dev/null 2>"$WORKDIR/$name.decode.log"; then
		cat "$WORKDIR/$name.decode.log" >&2
		fail "$name: output could not be decoded"
	fi
	if test -s "$WORKDIR/$name.decode.log"; then
		cat "$WORKDIR/$name.decode.log" >&2
		fail "$name: decoder reported warnings or errors"
	fi
}

assert_streams() {
	name=$1
	file=$2
	expected=$3
	actual=$($FFPROBE -v error -show_entries stream=codec_type \
			-of csv=p=0 "$file")
	test "$actual" = "$expected" ||
		fail "$name: expected streams '$expected', got '$actual'"
}

assert_frame_count() {
	name=$1
	file=$2
	expected=$3
	actual=$($FFPROBE -v error -select_streams v:0 -count_frames \
			-show_entries stream=nb_read_frames -of csv=p=0 "$file")
	test "$actual" = "$expected" ||
		fail "$name: expected $expected video frames, got $actual"
}

assert_duration() {
	name=$1
	file=$2
	minimum=$3
	maximum=$4
	actual=$($FFPROBE -v error -show_entries format=duration \
			-of default=noprint_wrappers=1:nokey=1 "$file")
	awk -v value="$actual" -v minimum="$minimum" -v maximum="$maximum" \
		'BEGIN { exit !(minimum <= value && value <= maximum) }' ||
		fail "$name: duration $actual is outside $minimum..$maximum seconds"
}

assert_video_span() {
	name=$1
	file=$2
	minimum=$3
	maximum=$4
	actual=$($FFPROBE -v error -select_streams v:0 \
			-show_entries frame=best_effort_timestamp_time,duration_time \
			-of csv=p=0 "$file" | awk -F, '
			NR == 1 { first = $1 }
			{ last = $1 + $2 }
			END { printf "%.6f", last - first }
		')
	awk -v value="$actual" -v minimum="$minimum" -v maximum="$maximum" \
		'BEGIN { exit !(minimum <= value && value <= maximum) }' ||
		fail "$name: video span $actual is outside $minimum..$maximum seconds"
}

require_command "$FFMPEG"
require_command "$FFPROBE"
test -x "$AVCUT" || fail "avcut executable not found: $AVCUT"

printf '%s\n' 'Generating H.264 fixtures...'
"$FFMPEG" -hide_banner -loglevel error -y \
	-f lavfi -i testsrc2=size=320x180:rate=25:duration=6 \
	-f lavfi -i sine=frequency=440:sample_rate=48000:duration=6 \
	-map 0:v -map 1:a -c:v libx264 -g 50 -bf 3 -c:a aac \
	"$WORKDIR/standard.mkv"

"$FFMPEG" -hide_banner -loglevel error -y \
	-f lavfi -i testsrc2=size=320x180:rate=25:duration=6 \
	-f lavfi -i sine=frequency=880:sample_rate=48000:duration=6 \
	-map 1:a -map 0:v -c:v libx264 -g 50 -bf 3 -c:a aac \
	"$WORKDIR/audio-first.mkv"

"$FFMPEG" -hide_banner -loglevel error -y \
	-f lavfi -i testsrc2=size=320x180:rate=25:duration=6 \
	-f lavfi -i sine=frequency=660:sample_rate=48000:duration=6 \
	-map 0:v -map 1:a -c:v libx264 -g 50 -bf 3 -c:a aac \
	-f mpegts "$WORKDIR/annexb.ts"

printf '%s\n' 'Testing no-cut passthrough...'
run_avcut no-cut -i "$WORKDIR/standard.mkv" \
	-o "$WORKDIR/no-cut.mkv"
assert_clean_decode no-cut "$WORKDIR/no-cut.mkv"
assert_streams no-cut "$WORKDIR/no-cut.mkv" "$(printf 'video\naudio')"
assert_frame_count no-cut "$WORKDIR/no-cut.mkv" 150
assert_duration no-cut "$WORKDIR/no-cut.mkv" 5.95 6.10

printf '%s\n' 'Testing H.264 cut with audio...'
run_avcut standard -i "$WORKDIR/standard.mkv" \
	-o "$WORKDIR/standard-cut.mkv" 1.3 2.7
assert_clean_decode standard "$WORKDIR/standard-cut.mkv"
assert_streams standard "$WORKDIR/standard-cut.mkv" "$(printf 'video\naudio')"
assert_frame_count standard "$WORKDIR/standard-cut.mkv" 115
assert_duration standard "$WORKDIR/standard-cut.mkv" 4.60 4.70

printf '%s\n' 'Testing skipped leading stream mapping...'
run_avcut skipped-stream -i "$WORKDIR/audio-first.mkv" \
	-o "$WORKDIR/audio-first-cut.mkv" -s 0 1.3 2.7
assert_clean_decode skipped-stream "$WORKDIR/audio-first-cut.mkv"
assert_streams skipped-stream "$WORKDIR/audio-first-cut.mkv" video
assert_frame_count skipped-stream "$WORKDIR/audio-first-cut.mkv" 115
assert_duration skipped-stream "$WORKDIR/audio-first-cut.mkv" 4.55 4.65

printf '%s\n' 'Testing trim from start...'
run_avcut trim-start -i "$WORKDIR/standard.mkv" \
	-o "$WORKDIR/trim-start.mkv" 0 1.3
assert_clean_decode trim-start "$WORKDIR/trim-start.mkv"
assert_streams trim-start "$WORKDIR/trim-start.mkv" "$(printf 'video\naudio')"
assert_frame_count trim-start "$WORKDIR/trim-start.mkv" 117
assert_duration trim-start "$WORKDIR/trim-start.mkv" 4.65 4.80

printf '%s\n' 'Testing open-ended trim at EOF...'
run_avcut trim-end -i "$WORKDIR/standard.mkv" \
	-o "$WORKDIR/trim-end.mkv" 4.6 -
assert_clean_decode trim-end "$WORKDIR/trim-end.mkv"
assert_streams trim-end "$WORKDIR/trim-end.mkv" "$(printf 'video\naudio')"
assert_frame_count trim-end "$WORKDIR/trim-end.mkv" 115
assert_duration trim-end "$WORKDIR/trim-end.mkv" 4.55 4.70

printf '%s\n' 'Testing multiple disjoint cuts...'
run_avcut multiple-cuts -i "$WORKDIR/standard.mkv" \
	-o "$WORKDIR/multiple-cuts.mkv" 0.8 1.2 3.3 4.1
assert_clean_decode multiple-cuts "$WORKDIR/multiple-cuts.mkv"
assert_streams multiple-cuts "$WORKDIR/multiple-cuts.mkv" "$(printf 'video\naudio')"
assert_frame_count multiple-cuts "$WORKDIR/multiple-cuts.mkv" 120
assert_duration multiple-cuts "$WORKDIR/multiple-cuts.mkv" 4.75 4.90

printf '%s\n' 'Testing complete GOP removal...'
run_avcut complete-gop -i "$WORKDIR/standard.mkv" \
	-o "$WORKDIR/complete-gop.mkv" 2 4
assert_clean_decode complete-gop "$WORKDIR/complete-gop.mkv"
assert_streams complete-gop "$WORKDIR/complete-gop.mkv" "$(printf 'video\naudio')"
assert_frame_count complete-gop "$WORKDIR/complete-gop.mkv" 100
assert_duration complete-gop "$WORKDIR/complete-gop.mkv" 3.95 4.10

printf '%s\n' 'Testing Annex B input...'
run_avcut annexb -i "$WORKDIR/annexb.ts" \
	-o "$WORKDIR/annexb-cut.mkv" 2.8 4.2
assert_clean_decode annexb "$WORKDIR/annexb-cut.mkv"
assert_streams annexb "$WORKDIR/annexb-cut.mkv" "$(printf 'video\naudio')"
assert_frame_count annexb "$WORKDIR/annexb-cut.mkv" 115
assert_video_span annexb "$WORKDIR/annexb-cut.mkv" 4.55 4.65

printf '%s\n' 'PASS: all integration tests passed'
