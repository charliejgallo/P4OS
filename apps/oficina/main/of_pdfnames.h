/*
 * P4OS - Office: the names PostScript fonts share (Adobe's Technical Note
 * 5176, appendices A and B): CFF's 391 standard strings, and Standard
 * Encoding's codes as their indices.
 */
#pragma once

#include <stdint.h>

static const char OF_CFF_STD[] =
    ".notdef\0" "space\0" "exclam\0" "quotedbl\0" "numbersign\0" "dollar\0" "percent\0" "ampersand\0"
    "quoteright\0" "parenleft\0" "parenright\0" "asterisk\0" "plus\0" "comma\0" "hyphen\0" "period\0"
    "slash\0" "zero\0" "one\0" "two\0" "three\0" "four\0" "five\0" "six\0" "seven\0" "eight\0" "nine\0"
    "colon\0" "semicolon\0" "less\0" "equal\0" "greater\0" "question\0" "at\0" "A\0" "B\0" "C\0" "D\0" "E\0"
    "F\0" "G\0" "H\0" "I\0" "J\0" "K\0" "L\0" "M\0" "N\0" "O\0" "P\0" "Q\0" "R\0" "S\0" "T\0" "U\0" "V\0"
    "W\0" "X\0" "Y\0" "Z\0" "bracketleft\0" "backslash\0" "bracketright\0" "asciicircum\0" "underscore\0"
    "quoteleft\0" "a\0" "b\0" "c\0" "d\0" "e\0" "f\0" "g\0" "h\0" "i\0" "j\0" "k\0" "l\0" "m\0" "n\0" "o\0"
    "p\0" "q\0" "r\0" "s\0" "t\0" "u\0" "v\0" "w\0" "x\0" "y\0" "z\0" "braceleft\0" "bar\0" "braceright\0"
    "asciitilde\0" "exclamdown\0" "cent\0" "sterling\0" "fraction\0" "yen\0" "florin\0" "section\0"
    "currency\0" "quotesingle\0" "quotedblleft\0" "guillemotleft\0" "guilsinglleft\0" "guilsinglright\0"
    "fi\0" "fl\0" "endash\0" "dagger\0" "daggerdbl\0" "periodcentered\0" "paragraph\0" "bullet\0"
    "quotesinglbase\0" "quotedblbase\0" "quotedblright\0" "guillemotright\0" "ellipsis\0" "perthousand\0"
    "questiondown\0" "grave\0" "acute\0" "circumflex\0" "tilde\0" "macron\0" "breve\0" "dotaccent\0"
    "dieresis\0" "ring\0" "cedilla\0" "hungarumlaut\0" "ogonek\0" "caron\0" "emdash\0" "AE\0" "ordfeminine\0"
    "Lslash\0" "Oslash\0" "OE\0" "ordmasculine\0" "ae\0" "dotlessi\0" "lslash\0" "oslash\0" "oe\0"
    "germandbls\0" "onesuperior\0" "logicalnot\0" "mu\0" "trademark\0" "Eth\0" "onehalf\0" "plusminus\0"
    "Thorn\0" "onequarter\0" "divide\0" "brokenbar\0" "degree\0" "thorn\0" "threequarters\0" "twosuperior\0"
    "registered\0" "minus\0" "eth\0" "multiply\0" "threesuperior\0" "copyright\0" "Aacute\0" "Acircumflex\0"
    "Adieresis\0" "Agrave\0" "Aring\0" "Atilde\0" "Ccedilla\0" "Eacute\0" "Ecircumflex\0" "Edieresis\0"
    "Egrave\0" "Iacute\0" "Icircumflex\0" "Idieresis\0" "Igrave\0" "Ntilde\0" "Oacute\0" "Ocircumflex\0"
    "Odieresis\0" "Ograve\0" "Otilde\0" "Scaron\0" "Uacute\0" "Ucircumflex\0" "Udieresis\0" "Ugrave\0"
    "Yacute\0" "Ydieresis\0" "Zcaron\0" "aacute\0" "acircumflex\0" "adieresis\0" "agrave\0" "aring\0"
    "atilde\0" "ccedilla\0" "eacute\0" "ecircumflex\0" "edieresis\0" "egrave\0" "iacute\0" "icircumflex\0"
    "idieresis\0" "igrave\0" "ntilde\0" "oacute\0" "ocircumflex\0" "odieresis\0" "ograve\0" "otilde\0"
    "scaron\0" "uacute\0" "ucircumflex\0" "udieresis\0" "ugrave\0" "yacute\0" "ydieresis\0" "zcaron\0"
    "exclamsmall\0" "Hungarumlautsmall\0" "dollaroldstyle\0" "dollarsuperior\0" "ampersandsmall\0"
    "Acutesmall\0" "parenleftsuperior\0" "parenrightsuperior\0" "twodotenleader\0" "onedotenleader\0"
    "zerooldstyle\0" "oneoldstyle\0" "twooldstyle\0" "threeoldstyle\0" "fouroldstyle\0" "fiveoldstyle\0"
    "sixoldstyle\0" "sevenoldstyle\0" "eightoldstyle\0" "nineoldstyle\0" "commasuperior\0"
    "threequartersemdash\0" "periodsuperior\0" "questionsmall\0" "asuperior\0" "bsuperior\0" "centsuperior\0"
    "dsuperior\0" "esuperior\0" "isuperior\0" "lsuperior\0" "msuperior\0" "nsuperior\0" "osuperior\0"
    "rsuperior\0" "ssuperior\0" "tsuperior\0" "ff\0" "ffi\0" "ffl\0" "parenleftinferior\0"
    "parenrightinferior\0" "Circumflexsmall\0" "hyphensuperior\0" "Gravesmall\0" "Asmall\0" "Bsmall\0"
    "Csmall\0" "Dsmall\0" "Esmall\0" "Fsmall\0" "Gsmall\0" "Hsmall\0" "Ismall\0" "Jsmall\0" "Ksmall\0"
    "Lsmall\0" "Msmall\0" "Nsmall\0" "Osmall\0" "Psmall\0" "Qsmall\0" "Rsmall\0" "Ssmall\0" "Tsmall\0"
    "Usmall\0" "Vsmall\0" "Wsmall\0" "Xsmall\0" "Ysmall\0" "Zsmall\0" "colonmonetary\0" "onefitted\0"
    "rupiah\0" "Tildesmall\0" "exclamdownsmall\0" "centoldstyle\0" "Lslashsmall\0" "Scaronsmall\0"
    "Zcaronsmall\0" "Dieresissmall\0" "Brevesmall\0" "Caronsmall\0" "Dotaccentsmall\0" "Macronsmall\0"
    "figuredash\0" "hypheninferior\0" "Ogoneksmall\0" "Ringsmall\0" "Cedillasmall\0" "questiondownsmall\0"
    "oneeighth\0" "threeeighths\0" "fiveeighths\0" "seveneighths\0" "onethird\0" "twothirds\0"
    "zerosuperior\0" "foursuperior\0" "fivesuperior\0" "sixsuperior\0" "sevensuperior\0" "eightsuperior\0"
    "ninesuperior\0" "zeroinferior\0" "oneinferior\0" "twoinferior\0" "threeinferior\0" "fourinferior\0"
    "fiveinferior\0" "sixinferior\0" "seveninferior\0" "eightinferior\0" "nineinferior\0" "centinferior\0"
    "dollarinferior\0" "periodinferior\0" "commainferior\0" "Agravesmall\0" "Aacutesmall\0"
    "Acircumflexsmall\0" "Atildesmall\0" "Adieresissmall\0" "Aringsmall\0" "AEsmall\0" "Ccedillasmall\0"
    "Egravesmall\0" "Eacutesmall\0" "Ecircumflexsmall\0" "Edieresissmall\0" "Igravesmall\0" "Iacutesmall\0"
    "Icircumflexsmall\0" "Idieresissmall\0" "Ethsmall\0" "Ntildesmall\0" "Ogravesmall\0" "Oacutesmall\0"
    "Ocircumflexsmall\0" "Otildesmall\0" "Odieresissmall\0" "OEsmall\0" "Oslashsmall\0" "Ugravesmall\0"
    "Uacutesmall\0" "Ucircumflexsmall\0" "Udieresissmall\0" "Yacutesmall\0" "Thornsmall\0" "Ydieresissmall\0"
    "001.000\0" "001.001\0" "001.002\0" "001.003\0" "Black\0" "Bold\0" "Book\0" "Light\0" "Medium\0"
    "Regular\0" "Roman\0" "Semibold\0";

static const uint16_t OF_CFF_STD_OFF[391] = {
    0, 8, 14, 21, 30, 41, 48, 56, 66, 77, 87, 98, 107, 112, 118, 125,
    132, 138, 143, 147, 151, 157, 162, 167, 171, 177, 183, 188, 194, 204, 209, 215,
    223, 232, 235, 237, 239, 241, 243, 245, 247, 249, 251, 253, 255, 257, 259, 261,
    263, 265, 267, 269, 271, 273, 275, 277, 279, 281, 283, 285, 287, 299, 309, 322,
    334, 345, 355, 357, 359, 361, 363, 365, 367, 369, 371, 373, 375, 377, 379, 381,
    383, 385, 387, 389, 391, 393, 395, 397, 399, 401, 403, 405, 407, 417, 421, 432,
    443, 454, 459, 468, 477, 481, 488, 496, 505, 517, 530, 544, 558, 573, 576, 579,
    586, 593, 603, 618, 628, 635, 650, 663, 677, 692, 701, 713, 726, 732, 738, 749,
    755, 762, 768, 778, 787, 792, 800, 813, 820, 826, 833, 836, 848, 855, 862, 865,
    878, 881, 890, 897, 904, 907, 918, 930, 941, 944, 954, 958, 966, 976, 982, 993,
    1000, 1010, 1017, 1023, 1037, 1049, 1060, 1066, 1070, 1079, 1093, 1103, 1110, 1122, 1132, 1139,
    1145, 1152, 1161, 1168, 1180, 1190, 1197, 1204, 1216, 1226, 1233, 1240, 1247, 1259, 1269, 1276,
    1283, 1290, 1297, 1309, 1319, 1326, 1333, 1343, 1350, 1357, 1369, 1379, 1386, 1392, 1399, 1408,
    1415, 1427, 1437, 1444, 1451, 1463, 1473, 1480, 1487, 1494, 1506, 1516, 1523, 1530, 1537, 1544,
    1556, 1566, 1573, 1580, 1590, 1597, 1609, 1627, 1642, 1657, 1672, 1683, 1701, 1720, 1735, 1750,
    1763, 1775, 1787, 1801, 1814, 1827, 1839, 1853, 1867, 1880, 1894, 1914, 1929, 1943, 1953, 1963,
    1976, 1986, 1996, 2006, 2016, 2026, 2036, 2046, 2056, 2066, 2076, 2079, 2083, 2087, 2105, 2124,
    2140, 2155, 2166, 2173, 2180, 2187, 2194, 2201, 2208, 2215, 2222, 2229, 2236, 2243, 2250, 2257,
    2264, 2271, 2278, 2285, 2292, 2299, 2306, 2313, 2320, 2327, 2334, 2341, 2348, 2362, 2372, 2379,
    2390, 2406, 2419, 2431, 2443, 2455, 2469, 2480, 2491, 2506, 2518, 2529, 2544, 2556, 2566, 2579,
    2597, 2607, 2620, 2632, 2645, 2654, 2664, 2677, 2690, 2703, 2715, 2729, 2743, 2756, 2769, 2781,
    2793, 2807, 2820, 2833, 2845, 2859, 2873, 2886, 2899, 2914, 2929, 2943, 2955, 2967, 2984, 2996,
    3011, 3022, 3030, 3044, 3056, 3068, 3085, 3100, 3112, 3124, 3141, 3156, 3165, 3177, 3189, 3201,
    3218, 3230, 3245, 3253, 3265, 3277, 3289, 3306, 3321, 3333, 3344, 3359, 3367, 3375, 3383, 3391,
    3397, 3402, 3407, 3413, 3420, 3428, 3434,
};

/* Standard Encoding: code -> standard string (0: none) */
static const uint8_t OF_STD_ENC[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80,
    81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
    0, 111, 112, 113, 114, 0, 115, 116, 117, 118, 119, 120, 121, 122, 0, 123,
    0, 124, 125, 126, 127, 128, 129, 130, 131, 0, 132, 133, 0, 134, 135, 136,
    137, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 138, 0, 139, 0, 0, 0, 0, 140, 141, 142, 143, 0, 0, 0, 0,
    0, 144, 0, 0, 0, 145, 0, 0, 146, 147, 148, 149, 0, 0, 0, 0,
};
