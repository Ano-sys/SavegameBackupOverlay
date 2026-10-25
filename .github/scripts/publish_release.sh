#!/usr/bin/env bash
set -euo pipefail

version=${1:?Pass the app version.}
artifact_directory=${2:-dist}

if [[ ! "$version" =~ ^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$ ]]; then
    echo "Invalid release version: $version" >&2
    exit 1
fi

: "${GITHUB_SHA:?Missing release commit.}"
: "${GITHUB_REPOSITORY:?Missing GitHub repository.}"
: "${GH_TOKEN:?Missing GitHub token.}"

tag="v$version"
artifact="$artifact_directory/SavegameBackupOverlay.ovl"
checksum="$artifact.sha256"

test -s "$artifact"
test -s "$checksum"

(
    cd "$artifact_directory"
    sha256sum --check SavegameBackupOverlay.ovl.sha256
)

release_error=$(mktemp)
trap 'rm -f -- "$release_error"' EXIT

if release_state=$(gh release view "$tag" --repo "$GITHUB_REPOSITORY" --json isDraft,targetCommitish --jq '[.isDraft, .targetCommitish] | @tsv' 2>"$release_error"); then
    read -r is_draft target_commit <<< "$release_state"

    if [[ "$is_draft" == "false" ]]; then
        echo "$tag is already published. Skipping release."
        exit 0
    fi

    if [[ "$is_draft" != "true" || "$target_commit" != "$GITHUB_SHA" ]]; then
        echo "The existing draft does not belong to this commit. Refusing to replace it." >&2
        exit 1
    fi
fi

if git show-ref --verify --quiet "refs/tags/$tag"; then
    tag_commit=$(git rev-parse "refs/tags/$tag^{commit}")

    if [[ "$tag_commit" != "$GITHUB_SHA" ]]; then
        echo "$tag points to another commit. Refusing to move the tag." >&2
        exit 1
    fi
fi

if [[ "${is_draft:-}" == "true" ]]; then
    gh release upload "$tag" "$artifact" "$checksum" --repo "$GITHUB_REPOSITORY" --clobber
    gh release edit "$tag" --repo "$GITHUB_REPOSITORY" --draft=false
else
    gh release create "$tag" "$artifact" "$checksum" --repo "$GITHUB_REPOSITORY" --target "$GITHUB_SHA" --title "Switch Cloud Backup $tag" --generate-notes
fi
