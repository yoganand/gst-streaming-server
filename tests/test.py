#!/usr/bin/env python

import httplib
import hashlib
import os
import sys
import time


clear_enabled=True

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
  print 'test_get_checksum("%s", "%s")' % (url, m.hexdigest())
  assert m.hexdigest() == expected_checksum

def test_get_checksum_range(url, start, end, expected_checksum):
  print ("Testing \"%s\"" % (url, ))
  conn = httplib.HTTPConnection("localhost", 8080)
  r = "bytes=%d-%d" % (start, end)
  conn.request("GET", url, "", {"Range": r})
  res = conn.getresponse()
  m = hashlib.sha1()
  m.update(res.read())
  assert res.status == 206
  print 'test_get_checksum_range("%s", %d, %d, "%s")' % (url, start, end, m.hexdigest())
  assert m.hexdigest() == expected_checksum


#args = ['../tools/gst-streaming-server']
#pid = os.spawnv(os.P_NOWAIT, "../tools/gst-streaming-server", args)
#print "PID is %d" % (pid,)

#time.sleep(2)

#if pid != 0:
#  sys.exit(1)

#sys.exit(0)


test_get_status("/", 200)

test_get_status("/vod", 404)
test_get_status("/vod/", 404)
test_get_status("/vod/", 404)
test_get_status("/vod/moo", 404)
test_get_status("/vod/elephantsdream", 404)
test_get_status("/vod/elephantsdream/", 404)
test_get_status("/vod/elephantsdream/0/pr", 404)
test_get_status("/vod/elephantsdream/0/pr/ism", 404)
test_get_status("/vod/broken/0/pr/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/Manifest", 200)

test_get_status("/vod/elephantsdream/broken/pr/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/1/pr/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/0/broken/ism/Manifest", 404)
test_get_status("/vod/elephantsdream/0/pr/broken/Manifest", 404)
test_get_status("/vod/elephantsdream/0/pr/ism/broken", 404)

test_get_status("/vod/broken/0/pr/ism/content", 404)

test_get_checksum("/vod/elephantsdream/0/pr/ism/Manifest", "ae130fc21ed2457226ac0b55af655e7ba907ed9e")
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

if clear_enabled:
  test_get_checksum("/vod/elephantsdream/0/clear/ism/Manifest", "c4ae5af94a7a983c3adddd4b1ef0f70802c3baa4")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=749643", "4dee4d4163d874db5566122476b5dea038e0b7df")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=1389589", "b750696e883d9cd064ffb24aa83ced7288f2b796")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=2338787", "4129295af4f9e18d09431d3d257cfd2c4f79ab25")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=4768805", "6419a2adf7cf996c86c883840e29ded0ba0f7492")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=audio&start_time=0&bitrate=131527", "cbfe0889f53cd60c9f54dd10656c11cedd331107")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=749643", "41a9b14300deb5de5404bcbf19cf5744c6011308")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=1389589", "fd66cbfd82d5afce8d0286655e0b100c4e506f93")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=2338787", "a17d30e6048393fd600e87ed7ecf3444e5f75d17")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=50416000&bitrate=4768805", "99bd9297f76c351bb2cd6da80f0265249735eb82")
  test_get_checksum("/vod/elephantsdream/0/clear/ism/content?stream=audio&start_time=50346666&bitrate=131527", "7a21c50cd4ab497f8b166f3e42a7b41a3d0aeef2")

  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=broken&start_time=0&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=broken&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=broken", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=10&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0&bitrate=10", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?start_time=0&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&bitrate=752428", 404)
  test_get_status("/vod/elephantsdream/0/clear/ism/content?stream=video&start_time=0", 404)

test_get_checksum("/vod/elephantsdream/0/pr/isoff-ondemand/manifest.mpd", "01c888d714940d88f76c93ad5a9cc34f8c5f0630")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/v0", 0, 655, "b6db0c49a705d2e478bd84a6f9dbf3f038751cf4")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/v0", 0, 655, "b6db0c49a705d2e478bd84a6f9dbf3f038751cf4")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/v0", 2248, 432537, "4d0841e089434993d459d26855baf8658ba411bc")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/a0", 0, 597, "809279bcd6eb3e933746d96a0747f0a434bd725e")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/a0", 598, 2189, "db68e9c679470bdcc03a9bc1042736ce5d3c59b2")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/a0", 2190, 85915, "ada346f0bb51bc8d4f96e55c75097c7451718e43")

if clear_enabled:
  test_get_checksum("/vod/elephantsdream/0/clear/isoff-ondemand/manifest.mpd", "3d049e6aee62576cf0f72f5f22c782ee62999a08")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/v0", 0, 655, "2f33c2079803b2657d23fb1e16e5fa85f9a349bc")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/v0", 656, 2247, "8615fb80b24584c4403ff884ef837cf0ec43ac52")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/v0", 2248, 432537, "0feb611df81f58b56c0fe1c1fc13cd51565224a6")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/a0", 0, 597, "7bd0e11ac2e6aa8a2cf4369a203a9c4e0dc0ece0")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/a0", 598, 2189, "2b838a08a33df152e56cdf87698c6c7acff01e7b")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/a0", 2190, 85915, "7321f26f7889f8ae45740ad9985af9cec606551b")



#os.kill(pid, 9)
#os.waitpid(pid, 0)



