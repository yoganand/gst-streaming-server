#!/usr/bin/env python

import httplib
import hashlib


def test_get_status(url, expected_status):
  print ("Testing \"%s\"" % (url, ))
  conn = httplib.HTTPConnection("localhost", 8080)
  conn.request("GET", url)
  res = conn.getresponse()
  data = res.read()
  assert res.status == expected_status

def test_get_checksum(url, expected_checksum):
  print ("Testing \"%s\"" % (url, ))
  conn = httplib.HTTPConnection("localhost", 8080)
  conn.request("GET", url)
  res = conn.getresponse()
  m = hashlib.sha1()
  m.update(res.read())
  #print 'test_get_checksum("%s", "%s")' % (url, m.hexdigest())
  assert m.hexdigest() == expected_checksum


test_get_status("/", 200)

test_get_status("/vod", 404)
test_get_status("/vod/", 404)
test_get_status("/vod/", 404)
test_get_status("/vod/moo", 404)
test_get_status("/vod/elephantsdream", 404)
test_get_status("/vod/elephantsdream/", 404)
test_get_status("/vod/elephantsdream/0/pr", 404)
test_get_status("/vod/elephantsdream/0/pr/ism", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/Manifest", 200)
test_get_status("/vod/broken/0/pr/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/Manifest", 200)

test_get_status("/vod/elephantsdream/broken/pr/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/1/pr/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/0/broken/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/0/pr/broken/Manifest", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/broken", 404)

test_get_status("/vod/broken/0/pr/ism/content", 404)

test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=752766", "7ac1f4eb8f6d3b1035aa404a4dc339a06f6d25ab")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=1392712", "460ad7ff103420b58a44e7d4635a2a0d4bf8e183")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=2341910", "bf2896df824d27009a06f96d0eac57d4c9f8e20d")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=4771928", "7fec77d6ff0369a4c5f022be48b3fe6b222c78cf")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=audio&start_time=0&bitrate=134578", "495bc12abc62eb80d8c4b4fbf35a748eb749b504")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=752766", "ea791edf3881289aa709e0e7c2015e1d87b80305")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=1392712", "09c8918b0dbe5aadc0a2c2fa465d23d5196a19f4")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=2341910", "a698362f8ab13b0e7e88387f99cb262f4cf7afa0")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=4771928", "d367b41aefc2f0d6cdcb0e4e416062d5b437ee7e")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=audio&start_time=50346666&bitrate=134578", "20a61d0c9abde21d19385d2a9230c755ce45c371")

test_get_status("/vod/elephantsdream/0/pr/ism/content?stream=broken&start_time=0&bitrate=752428", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=broken&bitrate=752428", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=broken", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=10&bitrate=752428", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=10", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/content?start_time=0&bitrate=752428", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/content?stream=video&bitrate=752428", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0", 404)

clear_enabled=False
if clear_enabled:
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=752766", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=1392712", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=2341910", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=4771928", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=audio&start_time=0&bitrate=134578", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=752766", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=1392712", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=2341910", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=4771928", 200)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=audio&start_time=50346666&bitrate=134578", 200)

  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=broken&start_time=0&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=broken&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=broken", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=10&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=10", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?start_time=0&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0", 404)

