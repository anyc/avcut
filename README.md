avcut
=====

avcut is a ffmpeg-based video cutter that is able to do frame-accurate cuts
of h264 streams (and possibly other codecs with or without inter-frame
prediction) without reencoding the complete stream.

avcut always buffers a so-called group of pictures (GOP) and either copies all
of the packets, if no cutting point lies in the corresponding time interval, or
reencodes the frames of the GOP before or after the cutting point in order to
resolve dependencies on frames that will be removed from the stream. Please
see [this blog article](http://kicherer.org/joomla/index.php/de/blog/42-avcut-frame-accurate-video-cutting-with-only-small-quality-loss)
for a detailed description.

_Please note:_

* This is an experimental version. Please check any resulting video if
  everything worked as intended. If you need a more mature solution, check
  [avidemux](http://fixounet.free.fr/avidemux/).
* You can specify an arbitrary output container that is supported by ffmpeg.
  However, avcut has only been tested with the Matroska (.mkv) container.

Dependencies
------------

* [ffmpeg](https://www.ffmpeg.org/)

Compilation
-----------

Install dependencies:

* For Ubuntu with ffmpeg:

  * `apt-get install libavcodec-dev libavformat-dev libavutil-dev`

* Gentoo:

  * emerge media-video/ffmpeg

Execute `make` to start building avcut. Execute `make debug` to build avcut with
verbose output and debug symbols.

Windows executable
------------------

An experimental Windows executable is provided as part of the CI artifacts.
This executable is dynamically linked against the ffmpeg libraries from
[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases). The
URL to the specific ffmpeg build is stored in the `avcut_ffmpeg_libs_url.txt`
file. Please place the DLLs from the bin/ folder of the ffmpeg build in the same
directory as avcut.exe.

Usage
-----

```
Usage: avcut [options] [<drop_from_ts> <continue_with_ts> ...]

Options:

  -c              Create a shell script to check cutpoints with mpv
  -d <diff>       Accept this difference in packet sizes during packet matching
  -e <encoder>    select video encoder by name manually
  -f <framecount> provide the approximate total frame count to show progress information
  -i <file>       Input file
  -o <file>       Output file
  -p <profile>    Use this encoding profile. If <profile> ends with ".profile",
                  <profile> is used as a path to the profile file. If not, the profile
                  is loaded from the default profile directory:
                     /usr/share/avcut/profiles/
  -s <index>      Skip stream with this index
  -v <level>      Set verbosity level (see https://www.ffmpeg.org/doxygen/2.8/log_8h.html)
```

Besides the input and output file, avcut expects a "blacklist", i.e. what should
be dropped, as argument. This blacklist consists of timestamps that denote from
where to where frames have to be dropped. The last argument can be a hyphen to
indicate that all remaining frames shall be dropped.

For example, to drop the frames of the first 10 seconds, the frames between
55.5s and 130s and all frames after 140s in input.avi and write the result to
output.mkv, the following command can be used:

`avcut -i input.avi -o output.mkv 0 10 55.5 130 140 -`

The option -c causes avcut to create a shell script in the current working directory
that will call [mpv](https://mpv.io/) to play the video from 10 seconds before to
10 seconds after each cutting point.
