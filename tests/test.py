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
  if m.hexdigest() != expected_checksum:
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
  if m.hexdigest() != expected_checksum:
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
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=752766", "987544b6952ca7e22a368f3a6b294d44d48b4ef3")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=1392712", "0ea4a488cce7c88c37290108bf6f190e1cebf2fc")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=2341910", "f8988711cf0ab5bce054c061079674479053aaed")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=0&bitrate=4771928", "461e0531ece8fd28dca46f9320fed75f7c80a698")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=audio&start_time=0&bitrate=134578", "495bc12abc62eb80d8c4b4fbf35a748eb749b504")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=752766", "217e1d7f61a5d2d3592460dfe1263a22e7d1c3a8")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=1392712", "7b5ff2df1927d83889df7a659b68fdadcdfd6554")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=2341910", "548291fc7965cb951b4b6734da4500c884a4debc")
test_get_checksum("/vod/elephantsdream/0/pr/ism/content?stream=video&start_time=50416000&bitrate=4771928", "3081b53c8bf237cee968e4f6a45af125ca629810")
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

test_get_checksum("/vod/elephantsdream/0/pr/isoff-ondemand/manifest.mpd", "a00a419a7f89f094200f965a428862cf01288863")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/v0", 0, 655, "3b25c1a6d5d3f8b4e4681f9c6b0a5b2269678144")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/v0", 0, 655, "3b25c1a6d5d3f8b4e4681f9c6b0a5b2269678144")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/v0", 2248, 432537, "9c8764f536d9954bb74147c24dd7dbe0bbf27892")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/a0", 0, 597, "6303d6b686daccc1122e94e236fb7daed50870ab")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/a0", 598, 2189, "70d999e5805cfd51371f735e6030e9ed5c0f8041")
test_get_checksum_range("/vod/elephantsdream/0/pr/isoff-ondemand/content/a0", 2190, 85915, "3abd1261886a65af156e182debeaba91be2eba87")

if clear_enabled:
  test_get_checksum("/vod/elephantsdream/0/clear/isoff-ondemand/manifest.mpd", "5bf139d40c70846930ee2f8b21fea59f6a4381c4")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/v0", 0, 655, "705d17a7e7f039ab555722bf3a5144be66101baf")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/v0", 656, 2247, "560ad64ddb05649bf120a7e9b11dc7eeca584b08")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/v0", 2248, 432537, "d479739ff6808c71d3be8eb7579f3ade5f20d568")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/a0", 0, 597, "5940fc3bfa99dd4a5f903edc382bc5c42f8e508a")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/a0", 598, 2189, "0fd77b2f3a0d12a082b781adc76b2af90b452b3c")
  test_get_checksum_range("/vod/elephantsdream/0/clear/isoff-ondemand/content/a0", 2190, 85915, "0c48e4cfc6a905e5ba58d6476eb032288ed055ab")



#os.kill(pid, 9)
#os.waitpid(pid, 0)



