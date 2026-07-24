#!/bin/sh
# Fetches the local transcription engine: the sherpa-onnx runtime and the
# Parakeet TDT 0.6B v3 model. install.sh calls this when you pick the local
# engine, but it stands alone and is safe to re-run -- every step is skipped
# if its output is already there, so a second run downloads nothing.
#
# Afterwards:  make WITH_PARAKEET=1 && sudo make install
# The flag is needed once. The Makefile records it and later builds keep it.
set -eu

PREFIX="${PREFIX:-/usr/local}"

# Pinned, never `latest`. sherpa-onnx has no ABI versioning -- the SONAME is
# unversioned, so the header and the .so must come from the same archive --
# and silently upgrading someone's transcriber underneath them is not an
# installer's call to make.
SHERPA_VERSION=v1.13.4
SHERPA_BASE="https://github.com/k2-fsa/sherpa-onnx/releases/download"

MODEL_NAME=parakeet-tdt-0.6b-v3-int8
MODEL_ARCHIVE=sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8
MODEL_URL="$SHERPA_BASE/asr-models/$MODEL_ARCHIVE.tar.bz2"

# The live preview's model. Parakeet cannot stream -- it is an offline model
# and NVIDIA publishes no streaming export -- so the preview that appears while
# you are still speaking comes from this small zipformer instead. It is less
# accurate, which costs nothing: Parakeet decodes the sealed utterance on
# release and its answer replaces the preview.
LIVE_NAME=streaming-zipformer-en
LIVE_ARCHIVE=sherpa-onnx-streaming-zipformer-en-2023-06-26
LIVE_URL="$SHERPA_BASE/asr-models/$LIVE_ARCHIVE.tar.bz2"
# Upstream names these after the training epoch and the chunk geometry. The
# daemon looks for the same four names it uses for Parakeet, so rename on the
# way in and keep asr_stream.c out of the business of tracking archive names.
LIVE_ENCODER=encoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx
LIVE_DECODER=decoder-epoch-99-avg-1-chunk-16-left-128.int8.onnx
LIVE_JOINER=joiner-epoch-99-avg-1-chunk-16-left-128.int8.onnx

LIB_DIR="$PREFIX/lib/scribe"
MODEL_DIR="$PREFIX/share/scribe/models/$MODEL_NAME"
LIVE_DIR="$PREFIX/share/scribe/models/$LIVE_NAME"
# The compile step needs the headers, and they must match the .so exactly, so
# they stay in the build tree rather than being installed system-wide.
INCLUDE_DIR="third_party/sherpa-onnx/include"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    %s\033[0m\n' "$1"; }
die()  { printf '\033[31merror: %s\033[0m\n' "$1" >&2; exit 1; }

[ -f Makefile ] && [ -d src ] || die "run this from the scribe source directory"
[ "$(id -u)" -ne 0 ] || die "do not run this as root; it will sudo where needed"
command -v curl >/dev/null || die "curl is required"
command -v tar  >/dev/null || die "tar is required"
# GNU tar does not decompress bzip2 itself; it execs the bzip2 binary. Without
# it `tar xjf` fails halfway through, after the 490 MB has been downloaded.
command -v bzip2 >/dev/null || die "bzip2 is required (tar shells out to it)"

case "$(uname -m)" in
    x86_64)  SHERPA_ARCHIVE="sherpa-onnx-$SHERPA_VERSION-linux-x64-shared-no-tts" ;;
    aarch64) SHERPA_ARCHIVE="sherpa-onnx-$SHERPA_VERSION-linux-aarch64-shared-cpu" ;;
    *)       die "no prebuilt sherpa-onnx for $(uname -m); build it from source
    and install libsherpa-onnx-c-api.so and libonnxruntime.so to $LIB_DIR,
    the headers to $INCLUDE_DIR, then run: make WITH_PARAKEET=1" ;;
esac
SHERPA_URL="$SHERPA_BASE/$SHERPA_VERSION/$SHERPA_ARCHIVE.tar.bz2"

# Note honestly: sherpa-onnx publishes no checksums for these assets, so HTTPS
# to GitHub is the whole trust boundary. A checksum computed from the file we
# just downloaded would verify nothing, so there is not one here.

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fetch() {
    _url="$1"; _out="$2"
    echo "    $_url"
    curl -fL --progress-bar -o "$_out" "$_url" || die "download failed: $_url"
}

# ---- runtime ---------------------------------------------------------------
say "sherpa-onnx $SHERPA_VERSION"
if [ -f "$LIB_DIR/libsherpa-onnx-c-api.so" ] && [ -d "$INCLUDE_DIR" ]; then
    echo "    already installed in $LIB_DIR, skipping"
else
    echo "    about to download about 25 MB"
    fetch "$SHERPA_URL" "$TMP/sherpa.tar.bz2"
    tar xjf "$TMP/sherpa.tar.bz2" -C "$TMP"
    SRC="$TMP/$SHERPA_ARCHIVE"
    [ -d "$SRC/lib" ] || die "unexpected archive layout in $SHERPA_ARCHIVE"

    # Both libraries land in one directory on purpose: libsherpa-onnx-c-api.so
    # carries RPATH=$ORIGIN, so it finds libonnxruntime.so beside it and no
    # ldconfig entry is needed.
    sudo install -Dm755 "$SRC/lib/libsherpa-onnx-c-api.so" "$LIB_DIR/libsherpa-onnx-c-api.so"
    sudo install -Dm755 "$SRC/lib/libonnxruntime.so"       "$LIB_DIR/libonnxruntime.so"
    echo "    installed the runtime to $LIB_DIR"

    rm -rf "$INCLUDE_DIR"
    mkdir -p "$(dirname "$INCLUDE_DIR")"
    cp -r "$SRC/include" "$INCLUDE_DIR"
    echo "    headers in $INCLUDE_DIR"
fi

# ---- model -----------------------------------------------------------------
say "Parakeet TDT 0.6B v3"
if [ -f "$MODEL_DIR/encoder.int8.onnx" ]; then
    echo "    already installed in $MODEL_DIR, skipping"
else
    echo "    about to download about 490 MB (about 640 MB on disk)"
    fetch "$MODEL_URL" "$TMP/model.tar.bz2"
    tar xjf "$TMP/model.tar.bz2" -C "$TMP"
    SRC="$TMP/$MODEL_ARCHIVE"
    [ -f "$SRC/encoder.int8.onnx" ] || die "unexpected archive layout in $MODEL_ARCHIVE"

    for f in encoder.int8.onnx decoder.int8.onnx joiner.int8.onnx tokens.txt; do
        sudo install -Dm644 "$SRC/$f" "$MODEL_DIR/$f"
    done

    # One sample survives as test.wav. The rest of test_wavs/ is dead weight,
    # but a single known-good utterance is what `make test-parakeet` decodes,
    # and it is the fastest way to tell a broken install from a bad microphone.
    # These are 22 and 24 kHz, not the 16 kHz scribe captures at; sherpa-onnx
    # resamples, so the test reads the rate from the file rather than assuming.
    SAMPLE="$SRC/test_wavs/en.wav"
    [ -f "$SAMPLE" ] || SAMPLE="$(find "$SRC/test_wavs" -name '*.wav' 2>/dev/null | sort | head -n 1)"
    if [ -n "$SAMPLE" ]; then
        sudo install -Dm644 "$SAMPLE" "$MODEL_DIR/test.wav"
    else
        warn "no sample WAV in the archive; make test-parakeet needs one of your own"
    fi
    echo "    installed the model to $MODEL_DIR"
fi

# ---- live model ------------------------------------------------------------
say "streaming zipformer (live preview)"
if [ -f "$LIVE_DIR/encoder.int8.onnx" ]; then
    echo "    already installed in $LIVE_DIR, skipping"
else
    echo "    about to download about 70 MB"
    fetch "$LIVE_URL" "$TMP/live.tar.bz2"
    tar xjf "$TMP/live.tar.bz2" -C "$TMP"
    SRC="$TMP/$LIVE_ARCHIVE"
    [ -f "$SRC/$LIVE_ENCODER" ] || die "unexpected archive layout in $LIVE_ARCHIVE"

    sudo install -Dm644 "$SRC/$LIVE_ENCODER" "$LIVE_DIR/encoder.int8.onnx"
    sudo install -Dm644 "$SRC/$LIVE_DECODER" "$LIVE_DIR/decoder.int8.onnx"
    sudo install -Dm644 "$SRC/$LIVE_JOINER"  "$LIVE_DIR/joiner.int8.onnx"
    sudo install -Dm644 "$SRC/tokens.txt"    "$LIVE_DIR/tokens.txt"

    # One 16 kHz sample survives as test.wav, for `make test-stream`. Unlike the
    # Parakeet test this one cannot resample: an online recognizer is fed at the
    # rate it was built for, so the sample has to already be at 16 kHz. These
    # are.
    SAMPLE="$SRC/test_wavs/0.wav"
    [ -f "$SAMPLE" ] || SAMPLE="$(find "$SRC/test_wavs" -name '*.wav' 2>/dev/null | sort | head -n 1)"
    if [ -n "$SAMPLE" ]; then
        sudo install -Dm644 "$SAMPLE" "$LIVE_DIR/test.wav"
    else
        warn "no sample WAV in the archive; make test-stream needs one of your own"
    fi
    echo "    installed the live model to $LIVE_DIR"
fi

# ---- done ------------------------------------------------------------------
say "ready"
cat <<EOF
    runtime   $LIB_DIR
    model     $MODEL_DIR
    live      $LIVE_DIR
    headers   $INCLUDE_DIR

Build and install the daemon against it:
    make WITH_PARAKEET=1 && sudo make install PREFIX=$PREFIX

Then set in ~/.config/scribe/config.ini:
    engine = parakeet

The transcriber is NVIDIA Parakeet TDT 0.6B v3, licensed CC-BY-4.0.
The live preview is a k2-fsa streaming zipformer, licensed Apache-2.0.
EOF
