# Copyright 1999-2015 Gentoo Foundation
# Distributed under the terms of the GNU General Public License v2
# $Id$

EAPI=5

if [ "${PV}" == "9999" ]; then
	inherit git-r3
fi

DESCRIPTION="Frame-accurate video cutting with only small quality loss"
HOMEPAGE="http://github.com/anyc/avcut.git"
if [ "${PV}" == "9999" ]; then
	EGIT_REPO_URI="https://github.com/anyc/avcut.git"
else
	SRC_URI="https://github.com/anyc/avcut/archive/avcut-${PV}.tar.gz"
fi
LICENSE="GPL-2"
SLOT="0"

KEYWORDS="~amd64 ~x86"
IUSE=""

RDEPEND="media-video/ffmpeg"
DEPEND="${RDEPEND}"

DOCS=( README.md )

src_install() {
	dobin avcut
}
