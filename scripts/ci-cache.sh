#!/usr/bin/env bash
# Keeps the ccache directory of the CI builds between runs.
#
# actions/cache can't do this here: the builds run on tag pushes, and a tag is
# its own cache scope (a run can only read the default branch's caches), so no
# tag build would ever see the cache of the previous one. Instead the cache is
# one asset (ccache.tar.zst) of a pre-release tagged "ci-cache" in this repo.
#
#   scripts/ci-cache.sh restore    # before the build; never fails the job
#   scripts/ci-cache.sh save       # after a successful build
#
# Needs GITHUB_TOKEN (contents: write), GITHUB_REPOSITORY, CCACHE_DIR, curl, jq, zstd.
set -uo pipefail

for tool in curl jq zstd; do
    command -v "$tool" >/dev/null || { echo "ci-cache: $tool missing, skipping"; exit 0; }
done
: "${GITHUB_TOKEN:?}" "${GITHUB_REPOSITORY:?}" "${CCACHE_DIR:?}"
API="https://api.github.com/repos/$GITHUB_REPOSITORY"
AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN" -H "X-GitHub-Api-Version: 2022-11-28")
NAME=ccache.tar.zst
TMP="${RUNNER_TEMP:-/tmp}/$NAME"

release_json() { curl -fsS "${AUTH[@]}" "$API/releases/tags/ci-cache"; }

case "${1:-}" in
restore)
    json=$(release_json) || { echo "ci-cache: no cache release yet (cold build)"; exit 0; }
    url=$(jq -r --arg n "$NAME" '.assets[]? | select(.name == $n) | .url' <<<"$json")
    [[ -n "$url" ]] || { echo "ci-cache: no cache asset yet (cold build)"; exit 0; }
    curl -fsSL "${AUTH[@]}" -H "Accept: application/octet-stream" "$url" -o "$TMP" \
        || { echo "ci-cache: download failed (cold build)"; exit 0; }
    mkdir -p "$(dirname "$CCACHE_DIR")"
    tar -I zstd -xf "$TMP" -C "$(dirname "$CCACHE_DIR")" \
        && echo "ci-cache: restored $(du -sh "$CCACHE_DIR" | cut -f1)" \
        || echo "ci-cache: unpack failed (cold build)"
    rm -f "$TMP"
    ;;
save)
    tar -I 'zstd -T0 -3' -cf "$TMP" -C "$(dirname "$CCACHE_DIR")" "$(basename "$CCACHE_DIR")" \
        || { echo "ci-cache: pack failed"; exit 0; }
    json=$(release_json) || json=$(curl -fsS "${AUTH[@]}" -X POST "$API/releases" -d \
        '{"tag_name":"ci-cache","name":"CI build cache (not a release)","body":"Compiler cache used by the CI builds. Not a release; do not download.","prerelease":true}') \
        || { echo "ci-cache: cannot create the cache release"; exit 0; }
    id=$(jq -r .id <<<"$json")
    for old in $(jq -r --arg n "$NAME" '.assets[]? | select(.name == $n) | .id' <<<"$json"); do
        curl -fsS "${AUTH[@]}" -X DELETE "$API/releases/assets/$old" >/dev/null
    done
    curl -fsS "${AUTH[@]}" -H "Content-Type: application/zstd" --data-binary "@$TMP" \
        "https://uploads.github.com/repos/$GITHUB_REPOSITORY/releases/$id/assets?name=$NAME" >/dev/null \
        && echo "ci-cache: saved $(du -h "$TMP" | cut -f1)" \
        || echo "ci-cache: upload failed"
    rm -f "$TMP"
    ;;
*)
    echo "usage: $0 restore|save" >&2
    exit 2
    ;;
esac
