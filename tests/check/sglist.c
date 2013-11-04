

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_VALGRIND_H
# include <valgrind/valgrind.h>
#else
# define RUNNING_ON_VALGRIND FALSE
#endif

#include "gst-streaming-server/gss-sglist.h"
#include <gst/check/gstcheck.h>

GST_START_TEST (test_sglist)
{
  GssSGList *sglist;

  sglist = gss_sglist_new (2);

  fail_unless (gss_sglist_get_size (sglist) == 0);

  sglist->chunks[0].size = 0x100;
  sglist->chunks[1].size = 0x100;

  fail_unless (gss_sglist_get_size (sglist) == 0x200);

  gss_sglist_merge (sglist);

  fail_unless (sglist->chunks[1].size == 0x100);

  sglist->chunks[1].offset = 0x100;

  gss_sglist_merge (sglist);

  fail_unless (sglist->chunks[0].size == 0x200);
  fail_unless (sglist->chunks[1].size == 0x0);

  gss_sglist_free (sglist);
}

GST_END_TEST;


static Suite *
gss_sglist_suite (void)
{
  Suite *s = suite_create ("GssSGList");
  TCase *tc_chain = tcase_create ("general");

  suite_add_tcase (s, tc_chain);
  tcase_add_test (tc_chain, test_sglist);

  return s;
}

GST_CHECK_MAIN (gss_sglist);
