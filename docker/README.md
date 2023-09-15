## bambi container distribution

Since bambi version 0.17.1 a Docker image is published matching each release.

### Using the container

Pull and run latest or any previously published image.

```bash
docker pull ghcr.io/wtsi-npg/bambi:latest
docker run ghcr.io/wtsi-npg/bambi:latest bambi --version
```

### Manual build

You should be able to build the image locally. You need to know which version
of HTSlib is compatible with the bambi version you are building.

```bash
git clone -b master https://github.com/wtsi-npg/bambi.git && cd bambi
export HTSLIB_BRANCH=1.18 # latest compatible version of HTSlib
docker build --rm \
  --file docker/Dockerfile \
  --build-arg HTSLIB_BRANCH="${HTSLIB_BRANCH}" \
  --tag bambi .
docker run bambi bambi
```

### Automatic build and publishing

Github actions is configured to build and publish a new image when a tag is
pushed to github. Docker is also invoked to build the container on push. If
you are preparing a new image we recommend tagging in your own fork and
pushing the tag to test things.

```bash
git clone -b your-branch https://github.com/your-fork/bambi.git && cd bambi
git tag -a '0.17.0.alpha.1' -m 'release 0.17.0.alpha.1'
git push origin 0.17.0.alpha.1
# wait for jobs to complete
docker pull ghcr.io/your-fork/bambi:latest
```