/*************************************************************************
	> File Name: my_misc.h
	> Author: JianWei
	> Mail: wj_clear@163.com 
	> Created Time: 2022年11月07日 星期一 15时47分38秒
 ************************************************************************/
#ifndef __MY_MISC_H__
#define __MY_MISC_H__

#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifndef MISC_EXIT_MODE
	#define MISC_EXIT_MODE 1
	#define MISC_LOG_MODE  0
#endif


#ifndef likely
	#define likely(x)   __builtin_expect(!!(x), 1)
	#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

#if defined(__GNUC__)
	#define force_inline  __attribute__((always_inline))
#else 
	#define force_inline
#endif

#define cache_line_64  __attribute__((aligned(64)))
#endif



