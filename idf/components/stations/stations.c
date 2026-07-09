#include "stations.h"

const station_t STATIONS[] = {
    {"TBS",     "TBSラジオ",         "TBS",  0xC62828},
    {"QRR",     "文化放送",           "文化", 0x1565C0},
    {"LFR",     "ニッポン放送",       "LFR",  0x2E7D32},
    {"RN1",     "ラジオNIKKEI第1",   "RN1",  0x0277BD},
    {"RN2",     "ラジオNIKKEI第2",   "RN2",  0x558B2F},
    {"INT",     "interfm",            "INT",  0xE65100},
    {"FMT",     "TOKYO FM",           "FM",   0x6A1B9A},
    {"FMJ",     "J-WAVE",             "JW",   0x00838F},
    {"JORF",    "ラジオ日本",         "RJ",   0x4E342E},
    {"BAYFM78", "BAYFM78",            "BAY",  0x00695C},
    {"NACK5",   "NACK5",              "N5",   0xAD1457},
    {"YFM",     "FMヨコハマ",         "YFM",  0x283593},
    {"IBS",     "LuckyFM 茨城放送",   "LFM",  0xBF360C},
    {"JOAK",    "NHK AM(東京)",       "NHK1", 0x37474F},
    {"JOAK-FM", "NHK FM(東京)",       "NHKF", 0x00695C},
};

const int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);
