/* Sphero R2-D2 BLE identity: UUIDs and the anti-DoS magic.
 *
 * Extracted from baoshi/claude-r2d2-buddy @ cca13ec, main/common.h:18-40
 * (MIT, recorded in docs/research/source-map.md:12). Values cross-check against
 * our own validated Python layer, mac-prototype/r2_probe.py:58-61, which cites
 * spherov2 and the freer2 reverse-engineering independently. Two implementations
 * agreeing is the bar CLAUDE.md sets for protocol constants.
 */
#pragma once

#define R2D2_NAME        "D2-"   /* scan matches any name with this prefix */

/* MAIN_SERVICE + its command characteristic (commands and responses) */
#define R2D2_MAIN_SVC    {0x21,0x21,0x6f,0x72,0x65,0x68,0x70,0x53,\
                          0x20,0x4f,0x4f,0x57,0x01,0x00,0x01,0x00}
#define R2D2_CMD_CHR     {0x21,0x21,0x6f,0x72,0x65,0x68,0x70,0x53,\
                          0x20,0x4f,0x4f,0x57,0x02,0x00,0x01,0x00}

/* CONNECT_SERVICE + notify handle and the magic-write characteristic */
#define R2D2_CONNECT_SVC {0x21,0x21,0x6f,0x72,0x65,0x68,0x70,0x53,\
                          0x20,0x4f,0x4f,0x57,0x01,0x00,0x02,0x00}
#define R2D2_HANDLE_CHR  {0x21,0x21,0x6f,0x72,0x65,0x68,0x70,0x53,\
                          0x20,0x4f,0x4f,0x57,0x02,0x00,0x02,0x00}
#define R2D2_CONNECT_CHR {0x21,0x21,0x6f,0x72,0x65,0x68,0x70,0x53,\
                          0x20,0x4f,0x4f,0x57,0x05,0x00,0x02,0x00}

#define R2D2_MAGIC       "usetheforce...band"

/* The Sphero packet constants that used to live here (SOP/EOP/FLAGS/DID/CID)
 * are gone deliberately. r2_packet.h owns them now, and two copies of a
 * protocol constant is how they drift apart. This file is BLE identity only:
 * who to look for, and which characteristics carry what. */
