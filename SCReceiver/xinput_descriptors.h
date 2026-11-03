/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2021 Jason Skuby (mytechtoybox.com)
 * Xbox configuration layout adapted from GP2040-CE via OpenPuck.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#pragma once
#include <stdint.h>

namespace xinput {
static const uint8_t descriptor[] = {
	9,4,0,0,2,0xff,0x5d,1,0,
	17,0x21,0,1,1,0x25,0x81,20,0,0,0,0,0x13,2,8,0,0,
	7,5,0x81,3,32,0,1, 7,5,2,3,32,0,8,
	9,4,1,0,4,0xff,0x5d,3,0,
	27,0x21,0,1,1,1,0x83,0x40,1,4,0x20,0x16,0x85,0,0,0,0,0,0,0x16,6,0,0,0,0,0,0,
	7,5,0x83,3,32,0,2, 7,5,4,3,32,0,4, 7,5,0x85,3,32,0,64, 7,5,6,3,32,0,16,
	9,4,2,0,1,0xff,0x5d,2,0, 9,0x21,0,1,1,0x22,0x86,3,0, 7,5,0x86,3,32,0,16,
	9,4,3,0,0,0xff,0xfd,0x13,4, 6,0x41,0,1,1,3,
};
static_assert(sizeof(descriptor) == 144, "Xbox configuration body must be 144 bytes");
static const char security_text[] =
	"Xbox Security Method 3, Version 1.00, \xa9 2005 Microsoft "
	"Corporation. All rights reserved.";
}
