"""Retry wrapper for the terrain tools' HTTP requests.

AWS's anonymous S3 endpoint resets connections under concurrency and
occasionally returns a throttling status. The terrain downloader and the
Copernicus tile-list fetch both wrap their requests in `retry` so a
transient failure backs off instead of aborting the command.
"""

import http.client
import random
import sys
import time
import urllib.error

#: Statuses worth retrying -- transient overload, not "no such object".
_RETRYABLE_STATUS = frozenset({408, 425, 429, 500, 502, 503, 504})

#: Transport-level failures worth retrying (a connection reset mid-body
#: surfaces as one of these).
_RETRYABLE_ERRORS = (ConnectionError, TimeoutError, http.client.HTTPException)

#: Per-request socket timeout, seconds. Pass to urlopen alongside retry.
TIMEOUT_S = 60


def retry(fn, *, label, attempts=6):
    """Call `fn`, retrying transient network failures with exponential
    backoff. A non-retryable HTTPError (e.g. 404) or any other exception
    propagates immediately."""
    delay = 1.0
    for attempt in range(1, attempts):
        try:
            return fn()
        except urllib.error.HTTPError as e:
            if e.code not in _RETRYABLE_STATUS:
                raise
        except (urllib.error.URLError, *_RETRYABLE_ERRORS):
            pass
        sleep_s = delay + random.uniform(0.0, 0.5)
        print(f"  {label}: transient network error, retry {attempt}/{attempts - 1} in {sleep_s:.0f}s",
              file=sys.stderr)
        time.sleep(sleep_s)
        delay = min(delay * 2, 30.0)
    return fn()
