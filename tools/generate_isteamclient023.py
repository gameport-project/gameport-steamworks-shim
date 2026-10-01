#!/usr/bin/env python3
"""Generates sdk_includes/isteamclient023.h from a locally supplied Steamworks SDK.

Usage: generate_isteamclient023.py <path to SDK's public/steam/isteamclient.h>

The header is derived from Valve's SDK, so it is generated locally and never committed.
"""
import re
import sys
from pathlib import Path

if len(sys.argv) != 2:
    sys.exit(__doc__)

src = Path(sys.argv[1]).read_text(errors="ignore")
match = re.search(r"class\s+ISteamClient\s*\{(.*?)\n\};", src, re.S)
if not match:
    sys.exit("ISteamClient class not found in the given header")
body = match.group(1)

key = "STEAM_PRIVATE_API("
while (i := body.find(key)) >= 0:
    j, depth = i + len(key), 1
    while depth and j < len(body):
        depth += (body[j] == "(") - (body[j] == ")")
        j += 1
    body = body[:i] + body[i + len(key):j - 1] + body[j:]
body = re.sub(r"\n\s*(private|public):\s*\n", "\n", body)

out = (
    "#ifndef ISTEAMCLIENT023_H\n#define ISTEAMCLIENT023_H\n"
    "#ifdef STEAM_WIN32\n#pragma once\n#endif\n\n"
    "class ISteamClient023\n{\npublic:" + body + "\n};\n\n#endif // ISTEAMCLIENT023_H\n"
)
dest = Path(__file__).resolve().parent.parent / "sdk_includes" / "isteamclient023.h"
dest.write_text(out)
print(f"wrote {dest} ({out.count('virtual ')} virtual methods)")
