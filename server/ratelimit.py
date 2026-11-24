"""Admission limits for console connections. Pure logic: every call takes
`now` (float seconds), nothing here sleeps or reads a clock.

ConnGate decides whether a new console connection may be served:
  - at most `per_ip` connections open at once from one address,
  - at most `new_per_min` connections accepted per minute from one address,
  - an address whose connections failed their hello `fails_per_min` times in
    a minute gets no new ones until that minute has passed,
  - at most `total` connections open at once on the whole server.
Only ACCEPTED connections count towards the per-minute numbers: a console
that is being turned away retries every few seconds, and counting those
retries would keep it locked out for as long as it kept trying.

A key of None is not limited per address (it still counts towards `total`):
mp_server uses it for this machine in local mode (bots, emulators, tests)
and for addresses that are known to stand for many clients at once.
"""

from collections import deque

WINDOW_S = 60.0


class ConnGate:
    def __init__(self, per_ip=8, new_per_min=20, fails_per_min=10, total=500):
        self.per_ip = per_ip
        self.new_per_min = new_per_min
        self.fails_per_min = fails_per_min
        self.total = total
        self.open = 0                # connections being served
        self._live = {}              # key -> open connections
        self._new = {}               # key -> deque of accept times
        self._fails = {}             # key -> deque of failed-hello times
        self._ops = 0

    @staticmethod
    def _recent(table, key, now):
        q = table.get(key)
        if q is None:
            return 0
        while q and not 0 <= now - q[0] < WINDOW_S:
            q.popleft()
        if not q:
            del table[key]
            return 0
        return len(q)

    def _sweep(self, now):
        """Forget addresses with nothing recent (bounds the tables even when
        every connection claims a new address)."""
        for table in (self._new, self._fails):
            for key in list(table):
                self._recent(table, key, now)

    def admit(self, key, now):
        """"" = serve it (the caller MUST call release(key) when the
        connection ends), else why not: server_full | too_many_failures |
        too_many_connections | too_many_new_connections."""
        self._ops += 1
        if self._ops % 256 == 0:
            self._sweep(now)
        if self.open >= self.total:
            return "server_full"
        if key is not None:
            if self._recent(self._fails, key, now) >= self.fails_per_min:
                return "too_many_failures"
            if self._live.get(key, 0) >= self.per_ip:
                return "too_many_connections"
            if self._recent(self._new, key, now) >= self.new_per_min:
                return "too_many_new_connections"
            self._new.setdefault(key, deque()).append(now)
            self._live[key] = self._live.get(key, 0) + 1
        self.open += 1
        return ""

    def release(self, key):
        self.open = max(0, self.open - 1)
        if key is not None:
            n = self._live.get(key, 0) - 1
            if n > 0:
                self._live[key] = n
            else:
                self._live.pop(key, None)

    def failed(self, key, now):
        """An admitted connection never got through hello (nothing sent, not
        the protocol, a refused id)."""
        if key is not None:
            q = self._fails.setdefault(key, deque(maxlen=self.fails_per_min * 4))
            q.append(now)
