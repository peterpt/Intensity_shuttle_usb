#include <stdio.h>

struct mode {
	char name[32];
	int value;
};

static const mode foo[] = {
	"NTSC        ", 0xe901,
	"NTSC        ", 0xe9c1,
	"NTSC        ", 0xe801,
	"NTSC 23.98  ", 0xe901,
	"PAL         ", 0xe909,
	"PAL 5:4     ", 0xe819,
	"1080p 23.98 ", 0xe8ad,
	"1080p 24    ", 0xe88b,
	"1080p 25    ", 0xe86b,
	"1080p 29.97 ", 0xe9ed,
	"1080p 30    ", 0xe9cb,
	"1080i 50    ", 0xe84b,
	"1080i 59.94 ", 0xe82d,
	"1080i 60    ", 0xe80b,
	"720p 50     ", 0xe94b,
	"720p 50     ", 0xe943,
	"720p 59.94  ", 0xe92d,
	"720p 59.94  ", 0xe925,
	"720p 60     ", 0xe90b,
};

int main(void)
{
	for (int i = 0; i < sizeof(foo) / sizeof(foo[0]); ++i) {
		int value = foo[i].value;
		printf("%-16s: mode=0x%04x, deep color=%d, dropframe=%d, hd_and_not_dropframe=%d, remainder=0x%04x\n",
			foo[i].name, value, !!(value & 0x8), !!(value & 0x4), !!(value & 0x2), value & ~(0xe800 | 0x8 | 0x4 | 0x2));
	}
}
