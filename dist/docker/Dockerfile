FROM gentoo/stage3-amd64
MAINTAINER https://github.com/laeubi

RUN mkdir -p /usr/portage && \
	emerge-webrsync && \
	emerge media-video/ffmpeg && \
	rm -rf /usr/portage
WORKDIR /build
CMD make
