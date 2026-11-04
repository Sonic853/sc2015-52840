"""Calendar release versions; numeric capability revisions remain independent."""

from datetime import datetime, timedelta, timezone
import re

VERSION = re.compile(r"(\d{4}\.\d{2}\.\d{2})(?:p([1-9]\d*))?", re.ASCII)


def validate_version(value):
    match = VERSION.fullmatch(value) if isinstance(value, str) else None
    if not match:
        raise ValueError("Expected YYYY.MM.DD or YYYY.MM.DDpN (N starts at 1)")
    datetime.strptime(match[1], "%Y.%m.%d")
    return value


def release_day(now=None):
    china = timezone(timedelta(hours=8))
    return (now or datetime.now(china)).astimezone(china).strftime("%Y.%m.%d")


def next_version(day, existing):
    validate_version(day)
    if "p" in day:
        raise ValueError("Release day cannot have a patch suffix")
    patches = []
    for name in existing:
        match = VERSION.fullmatch(name)
        if match and match[1] == day:
            patches.append(int(match[2] or 0))
    return f"{day}p{max(patches) + 1}" if patches else day
