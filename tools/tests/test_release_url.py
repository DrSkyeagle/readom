#!/usr/bin/env python3
"""The URL the entry downloads a release payload from.

It is the one thing in a release that only matters to the person this project is
for: somebody who can plug in a cable, has no payload on their drive, and taps
the entry. Nothing on this side fetches it, so a wrong URL is found by that
person and nobody else. V65 shipped with the file name in it twice.

The guard written after V65 checked the undecoded text for that one mistake.
These are the cases that got past it.
"""
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
NAME = 'readom-system-v67.img.zst'
GOOD = 'https://github.com/hbyxabc/readom/releases/download/v67'


def load():
    """build_launcher.py by path: it is a script, not an installed module."""
    spec = importlib.util.spec_from_file_location('pw3_build_launcher', ROOT / 'boot/build_launcher.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ACCEPTED = [
    (GOOD, GOOD + '/' + NAME, 'the directory an asset sits in'),
    (GOOD + '/', GOOD + '/' + NAME, 'and the same with a trailing slash'),
    ('https://github.com/a/b/releases/download/v1',
     'https://github.com/a/b/releases/download/v1/' + NAME, 'short owner and repo'),
    # The route the entry carries from 2026-10-30 (the owner's decision): one
    # address on this project's own domain, with what is behind it the Worker's
    # to arrange. Off github.com there is no shape to hold an address to, so
    # these are refused only by what is true of any address.
    ('https://readom.app/dl/v0.1.0', 'https://readom.app/dl/v0.1.0/' + NAME,
     "the project's own download route"),
    ('https://readom.app/dl/v0.1.0/', 'https://readom.app/dl/v0.1.0/' + NAME,
     'and the same with a trailing slash'),
    ('https://example.test/x', 'https://example.test/x/' + NAME, 'one segment is enough off github.com'),
    # The host is settled before it is used, so a spelling of github.com that is
    # not the literal string still counts as github.com. Until 2026-10-30 this
    # one was refused while the tag page on the same spelling was accepted.
    ('https://GITHUB.COM/a/b/releases/download/v1',
     'https://github.com/a/b/releases/download/v1/' + NAME, 'upper case is the same host'),
    # The guard against the asset's own URL counted the name inside the path,
    # which also counted the segments that merely begin with it. Both of these
    # are directories, and the name appears in neither of them.
    ('https://readom.app/' + NAME + '-backup/v1', 'https://readom.app/' + NAME + '-backup/v1/' + NAME,
     'a folder whose name begins with the asset name'),
    ('https://github.com/a/b/releases/download/' + NAME + '-rc',
     'https://github.com/a/b/releases/download/' + NAME + '-rc/' + NAME,
     'a GitHub tag whose name begins with the asset name'),
]

REFUSED = [
    (GOOD + '/' + NAME, 'the asset itself, which was V65: the name ends up in it twice'),
    (GOOD + '/android%2Dsystem%2Dv67.img.zst',
     'the asset itself, percent-encoded -- this is what got past the text guard'),
    ('https://github.com/hbyxabc/readom/releases/tag/v67',
     'the tag page, which is what a browser shows and nobody can download from'),
    ('https://github.com/hbyxabc/readom/releases', 'the releases page'),
    ('https://github.com/hbyxabc/readom', 'the repository'),
    ('http://github.com/hbyxabc/readom/releases/download/v67', 'not https'),
    ('https://githubb.com/hbyxabc/readom/releases/download/v67',
     'the GitHub release shape on another host, which is a typo in the host'),
    ('https://readom.app/dl/v0.1.0/' + NAME,
     'the asset itself off github.com: no shape refuses it, the duplicate-name guard does'),
    ('https://readom.app/dl/v0.1.0/' + NAME + '/',
     'the same with a trailing slash, which is stripped before the count'),
    ('https://readom.app', 'a host with no path at all'),
    ('https://readom.app/dl//v0.1.0', 'an empty segment off github.com'),
    ('https://readom.app/dl/../v0.1.0', 'a dot-dot segment off github.com'),
    ('http://readom.app/dl/v0.1.0', 'not https, off github.com'),
    ('https://readom.app/dl/v0.1.0?x=1', 'a query string, off github.com'),
    # Spellings of github.com that are not the literal string. All three got a
    # GitHub **tag page** past the shape check until 2026-10-30, because the
    # comparison was against the raw netloc: the host did not look like
    # github.com, so no shape was required of it.
    ('https://GITHUB.COM/a/b/releases/tag/v1', 'the tag page, with the host in upper case'),
    ('https://github.com./a/b/releases/tag/v1', 'the tag page, with a trailing dot on the host'),
    ('https://%67ithub.com/a/b/releases/tag/v1', 'the tag page, with the host percent-encoded'),
    ('https://github.com./a/b/releases/download/v1', 'a trailing dot on the host, right shape'),
    ('https://./x', 'a host that is a single dot'),
    ('https://readom.app../x', 'an empty label in the host'),
    ('https://user@readom.app/dl/v0.1.0', 'user information in the authority'),
    ('https://readom.app:443/dl/v0.1.0', 'a port'),
    ('https://[::1]/x', 'an address literal rather than a host name'),
    ('https://localhost/x', 'a host with one label'),
    # Percent-encoding in the path is refused outright rather than decoded and
    # judged, because one decode does not say what a server will do: `%2e%2e`
    # decodes to `..` and was caught, `%252e%252e` decodes to `%2e%2e` and was
    # not. A release path is a version and a folder name and needs none of it.
    ('https://readom.app/dl/%252e%252e/v1', 'a doubly encoded dot-dot'),
    ('https://readom.app/dl/%5c..%5c/v1', 'an encoded backslash'),
    ('https://readom.app/dl/%00/v1', 'an encoded NUL'),
    ('https://readom.app/dl/%zz/v1', 'an invalid percent escape'),
    ('https://readom.app/dl/v0.1.0\n', 'a trailing newline, which urlsplit would have dropped'),
    ('https://github.com/hbyxabc/readom/releases/download/', 'no tag'),
    ('https://github.com//Readom/releases/download/v67', 'no owner'),
    ('https://github.com/hbyxabc/readom/releases/download/v67?x=1', 'a query string'),
    ('https://github.com/hbyxabc/readom/releases/download/v67#x', 'a fragment'),
    ('https://github.com/hbyxabc/readom/releases/download/../v67', 'a dot-dot segment'),
    # The three below have the right number of segments, so they get past the
    # shape check and are refused by the thing each is for. Without them the
    # percent-decoding, the dot-segment test and the duplicate-name test could
    # each be removed with this test still reporting every other case right.
    ('https://github.com/hbyxabc/readom/releases/download/..',
     'a dot-dot as the tag itself -- six segments, so only the dot check refuses it'),
    ('https://github.com/hbyxabc/readom/releases/download/v67%2Fsub',
     'a slash hidden in the tag -- six segments until it is decoded'),
    ('https://github.com/hbyxabc/readom/releases/download/' + NAME,
     'the tag is the asset name, so the name would appear twice in the result'),
    ('https://github.com/hbyxabc/readom/releases/download/v67/sub', 'a segment too many'),
    ('https://github.com/hbyxabc/readom/releases/downloads/v67', '"downloads", not "download"'),
    ('https://user@github.com/a/b/releases/download/v1', 'credentials in the authority'),
    ('github.com/a/b/releases/download/v1', 'no scheme'),
    # A name no resolver will take is not a host, and an address built on it
    # fails on every device that tries it rather than here.
    ('https://' + 'a' * 64 + '.test/x', 'a label of 64 bytes, one past what DNS allows'),
    ('https://' + '.'.join(['a' * 63] * 4) + '/x', 'a host of 255 bytes, past what DNS allows'),
]


def main():
    url_of = load().release_asset_url
    problems = []
    for given, expected, why in ACCEPTED:
        try:
            got = url_of(given, NAME)
        except SystemExit as refusal:
            problems.append('refused %s (%s): %s' % (given, why, refusal))
            continue
        if got != expected:
            problems.append('%s (%s) became %s, wanted %s' % (given, why, got, expected))
    for given, why in REFUSED:
        try:
            got = url_of(given, NAME)
        except SystemExit:
            continue
        problems.append('accepted %s (%s) and built %s' % (given, why, got))

    if problems:
        print('PW3_RELEASE_URL_FAIL')
        for problem in problems:
            print('  ' + problem)
        return 1
    print('PW3_RELEASE_URL_OK accepted=%d refused=%d' % (len(ACCEPTED), len(REFUSED)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
