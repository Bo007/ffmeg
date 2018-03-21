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
#include <string>

const std::string FILE_PATH = "F:\\test_videos\\";
const std::string FILE_NAMES[] = 
{
	// long
	/*0*/"Pohozhdeniya_imperatora.mp4",

	// middle
	/*1*/"Ofenbach-Be_Mine(Official Video).mp4",
	/*2*/"Ofenbach2.mp4",
	/*3*/"Black Foxxes - Manic In Me.mp4",
	/*4*/"HeadingUpHigh.mp4",
	/*5*/"videoplayback.mp4"

	// short
	/*6*/"738994211.mp4",
	/*7*/"Ofenbach-Be_Mine(Official Video) (online-video-cutter.com).mp4",
	/*8*/"HeadingUpHigh (online-video-cutter.com).mp4",
	/*9*/"Black Foxxes - Manic In Me (online-video-cutter.com).mp4",
};


int main(int argc, char *argv[])
{
	// if we want have log files
	//freopen("output.txt", "w", stdout);
	//freopen("error.txt", "w", stderr);

	int index = 4;
	return eventLoop( ( FILE_PATH + FILE_NAMES[index] ).c_str() );
}
