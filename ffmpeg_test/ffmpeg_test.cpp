// tutorial05.c
// A pedagogical video player that really works!
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard,
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
// Use
//
// gcc -o tutorial05 tutorial05.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed,
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial04 myvideofile.mpg
//
// to play the video stream on your screen.

#include <stdio.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include "Player.h"

#include <vector>

int main(int argc, char *argv[])
{
	std::vector<char *>fileNames;
	fileNames.push_back("F:\\Pohozhdeniya_imperatora.mp4");
	fileNames.push_back("F:\\738994211.mp4");
	fileNames.push_back("F:\\Ofenbach-Be_Mine(Official Video).mp4");
	fileNames.push_back("F:\\Ofenbach2.mp4");

	int index = 3;

	return eventLoop(fileNames[index]);
}
