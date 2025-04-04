/*
 *------------------------------------------------------------------
 * Copyright (c) 2017 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *------------------------------------------------------------------
 */

#include <memory>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <setjmp.h>
#include <check.h>
#include <vapi/memclnt.api.vapi.h>
#include <vapi/vapi.hpp>
#include <vapi/vpe.api.vapi.hpp>
#include <vapi/interface.api.vapi.hpp>
#include <vapi/ip.api.vapi.hpp>
#include <fake.api.vapi.hpp>

DEFINE_VAPI_MSG_IDS_VPE_API_JSON;
DEFINE_VAPI_MSG_IDS_INTERFACE_API_JSON;
DEFINE_VAPI_MSG_IDS_IP_API_JSON;
DEFINE_VAPI_MSG_IDS_FAKE_API_JSON;

static char *app_name = nullptr;
static char *api_prefix = nullptr;
static bool use_uds = false;
static const int max_outstanding_requests = 32;
static const int response_queue_size = 32;

#define WAIT_FOR_RESPONSE(param, ret)      \
  do                                       \
    {                                      \
      ret = con.wait_for_response (param); \
    }                                      \
  while (ret == VAPI_EAGAIN)

using namespace vapi;

void verify_show_version_reply (const Show_version_reply &r)
{
  auto &p = r.get_payload ();
  printf ("show_version_reply: program: `%s', version: `%s', build directory: "
          "`%s', build date: `%s'\n",
          p.program, p.version, p.build_directory, p.build_date);
  ck_assert_str_eq ("vpe", (char *)p.program);
}

Connection con;

void setup (void)
{
  vapi_error_e rv =
    con.connect (app_name, api_prefix, max_outstanding_requests,
		 response_queue_size, true, use_uds);
  ck_assert_int_eq (VAPI_OK, rv);
}

void teardown (void)
{
  con.disconnect ();
}

START_TEST (test_show_version_1)
{
  printf ("--- Show version by reading response associated to request ---\n");
  Show_version sv (con);
  vapi_error_e rv = sv.execute ();
  ck_assert_int_eq (VAPI_OK, rv);
  WAIT_FOR_RESPONSE (sv, rv);
  ck_assert_int_eq (VAPI_OK, rv);
  auto &r = sv.get_response ();
  verify_show_version_reply (r);
}

END_TEST;

struct Show_version_cb
{
  Show_version_cb () : called{0} {};
  int called;
  vapi_error_e operator() (Show_version &sv)
  {
    auto &r = sv.get_response ();
    verify_show_version_reply (r);
    ++called;
    return VAPI_OK;
  }
};

START_TEST (test_show_version_2)
{
  printf ("--- Show version by getting a callback ---\n");
  Show_version_cb cb;
  Show_version sv (con, std::ref (cb));
  vapi_error_e rv = sv.execute ();
  ck_assert_int_eq (VAPI_OK, rv);
  con.dispatch (sv);
  ck_assert_int_eq (1, cb.called);
}

END_TEST;

START_TEST (test_loopbacks_1)
{
  printf ("--- Create/delete loopbacks by waiting for response ---\n");
  const auto num_ifs = 5;
  u8 mac_addresses[num_ifs][6];
  memset (&mac_addresses, 0, sizeof (mac_addresses));
  u32 sw_if_indexes[num_ifs];
  memset (&sw_if_indexes, 0xff, sizeof (sw_if_indexes));
  for (int i = 0; i < num_ifs; ++i)
    {
      memcpy (&mac_addresses[i], "\1\2\3\4\5\6", 6);
      mac_addresses[i][5] = i;
    }
  for (int i = 0; i < num_ifs; ++i)
    {
      Create_loopback cl (con);
      auto &p = cl.get_request ().get_payload ();
      memcpy (p.mac_address, mac_addresses[i], sizeof (p.mac_address));
      auto e = cl.execute ();
      ck_assert_int_eq (VAPI_OK, e);
      vapi_error_e rv;
      WAIT_FOR_RESPONSE (cl, rv);
      ck_assert_int_eq (VAPI_OK, rv);
      auto &rp = cl.get_response ().get_payload ();
      ck_assert_int_eq (0, rp.retval);
      sw_if_indexes[i] = rp.sw_if_index;
    }
  for (int i = 0; i < num_ifs; ++i)
    {
      printf ("Created loopback with MAC %02x:%02x:%02x:%02x:%02x:%02x --> "
              "sw_if_index %u\n",
              mac_addresses[i][0], mac_addresses[i][1], mac_addresses[i][2],
              mac_addresses[i][3], mac_addresses[i][4], mac_addresses[i][5],
              sw_if_indexes[i]);
    }

  { // new context
    bool seen[num_ifs] = {0};
    Sw_interface_dump d (con, 0);
    auto rv = d.execute ();
    ck_assert_int_eq (VAPI_OK, rv);
    WAIT_FOR_RESPONSE (d, rv);
    ck_assert_int_eq (VAPI_OK, rv);
    auto &rs = d.get_result_set ();
    for (auto &r : rs)
      {
        auto &p = r.get_payload ();
        for (int i = 0; i < num_ifs; ++i)
          {
            if (sw_if_indexes[i] == p.sw_if_index)
              {
                ck_assert_int_eq (0, seen[i]);
                seen[i] = true;
              }
          }
      }
    for (int i = 0; i < num_ifs; ++i)
      {
        ck_assert_int_eq (1, seen[i]);
      }
  }

  for (int i = 0; i < num_ifs; ++i)
    {
      Delete_loopback dl (con);
      dl.get_request ().get_payload ().sw_if_index = sw_if_indexes[i];
      auto rv = dl.execute ();
      ck_assert_int_eq (VAPI_OK, rv);
      WAIT_FOR_RESPONSE (dl, rv);
      ck_assert_int_eq (VAPI_OK, rv);
      auto &response = dl.get_response ();
      auto rp = response.get_payload ();
      ck_assert_int_eq (0, rp.retval);
      printf ("Deleted loopback with sw_if_index %u\n", sw_if_indexes[i]);
    }

  { // new context
    Sw_interface_dump d (con, 0);
    auto rv = d.execute ();
    ck_assert_int_eq (VAPI_OK, rv);
    WAIT_FOR_RESPONSE (d, rv);
    ck_assert_int_eq (VAPI_OK, rv);
    auto &rs = d.get_result_set ();
    for (auto &r : rs)
      {
        auto &p = r.get_payload ();
        for (int i = 0; i < num_ifs; ++i)
          {
            ck_assert_int_ne (sw_if_indexes[i], p.sw_if_index);
          }
      }
  }
}

END_TEST;

struct Create_loopback_cb
{
  Create_loopback_cb () : called{ 0 }, sw_if_index{ 0 }, seen{ false } {};
  int called;
  u32 sw_if_index;
  bool seen;
  vapi_error_e operator() (Create_loopback &cl)
  {
    auto &r = cl.get_response ();
    sw_if_index = r.get_payload ().sw_if_index;
    ++called;
    return VAPI_OK;
  }
};

struct Delete_loopback_cb
{
  Delete_loopback_cb () : called{ 0 }, sw_if_index{ 0 }, seen{ false } {};
  int called;
  u32 sw_if_index;
  bool seen;
  vapi_error_e operator() (Delete_loopback &dl)
  {
    auto &r = dl.get_response ();
    ck_assert_int_eq (0, r.get_payload ().retval);
    ++called;
    return VAPI_OK;
  }
};

template <int num_ifs> struct Sw_interface_dump_cb
{
  Sw_interface_dump_cb (std::array<Create_loopback_cb, num_ifs> &cbs)
      : called{0}, cbs{cbs} {};
  int called;
  std::array<Create_loopback_cb, num_ifs> &cbs;
  vapi_error_e operator() (Sw_interface_dump &d)
  {
    for (auto &y : cbs)
      {
        y.seen = false;
      }
    for (auto &x : d.get_result_set ())
      {
        auto &p = x.get_payload ();
        for (auto &y : cbs)
          {
            if (p.sw_if_index == y.sw_if_index)
              {
                y.seen = true;
              }
          }
      }
    for (auto &y : cbs)
      {
        ck_assert_int_eq (true, y.seen);
      }
    ++called;
    return VAPI_OK;
  }
};

START_TEST (test_loopbacks_2)
{
  printf ("--- Create/delete loopbacks by getting a callback ---\n");
  const auto num_ifs = 5;
  u8 mac_addresses[num_ifs][6];
  memset (&mac_addresses, 0, sizeof (mac_addresses));
  for (int i = 0; i < num_ifs; ++i)
    {
      memcpy (&mac_addresses[i], "\1\2\3\4\5\6", 6);
      mac_addresses[i][5] = i;
    }
  std::array<Create_loopback_cb, num_ifs> ccbs;
  std::array<std::unique_ptr<Create_loopback>, num_ifs> clcs;
  for (int i = 0; i < num_ifs; ++i)
    {
      Create_loopback *cl = new Create_loopback (con, std::ref (ccbs[i]));
      clcs[i].reset (cl);
      auto &p = cl->get_request ().get_payload ();
      memcpy (p.mac_address, mac_addresses[i], sizeof (p.mac_address));
      auto e = cl->execute ();
      ck_assert_int_eq (VAPI_OK, e);
    }
  con.dispatch ();
  for (int i = 0; i < num_ifs; ++i)
    {
      ck_assert_int_eq (1, ccbs[i].called);
      printf ("Created loopback with MAC %02x:%02x:%02x:%02x:%02x:%02x --> "
              "sw_if_index %u\n",
              mac_addresses[i][0], mac_addresses[i][1], mac_addresses[i][2],
              mac_addresses[i][3], mac_addresses[i][4], mac_addresses[i][5],
              ccbs[i].sw_if_index);
    }

  Sw_interface_dump_cb<num_ifs> swdcb (ccbs);
  Sw_interface_dump d (con, 0, std::ref (swdcb));
  auto rv = d.execute ();
  ck_assert_int_eq (VAPI_OK, rv);
  WAIT_FOR_RESPONSE (d, rv);
  ck_assert_int_eq (VAPI_OK, rv);
  ck_assert_int_ne (0, swdcb.called);
  std::array<Delete_loopback_cb, num_ifs> dcbs;
  std::array<std::unique_ptr<Delete_loopback>, num_ifs> dlcs;
  for (int i = 0; i < num_ifs; ++i)
    {
      Delete_loopback *dl = new Delete_loopback (con, std::ref (dcbs[i]));
      dlcs[i].reset (dl);
      auto &p = dl->get_request ().get_payload ();
      p.sw_if_index = ccbs[i].sw_if_index;
      dcbs[i].sw_if_index = ccbs[i].sw_if_index;
      auto e = dl->execute ();
      ck_assert_int_eq (VAPI_OK, e);
    }
  con.dispatch ();
  for (auto &x : dcbs)
    {
      ck_assert_int_eq (true, x.called);
      printf ("Deleted loopback with sw_if_index %u\n", x.sw_if_index);
    }

  { // new context
    Sw_interface_dump d (con, 0);
    auto rv = d.execute ();
    ck_assert_int_eq (VAPI_OK, rv);
    WAIT_FOR_RESPONSE (d, rv);
    ck_assert_int_eq (VAPI_OK, rv);
    auto &rs = d.get_result_set ();
    for (auto &r : rs)
      {
        auto &p = r.get_payload ();
        for (int i = 0; i < num_ifs; ++i)
          {
            ck_assert_int_ne (ccbs[i].sw_if_index, p.sw_if_index);
          }
      }
  }
}

END_TEST;

START_TEST (test_unsupported)
{
  printf ("--- Unsupported messages ---\n");
  bool thrown = false;
  try
    {
      Test_fake_msg fake (con);
    }
  catch (const Msg_not_available_exception &)
    {
      thrown = true;
      printf ("Constructing unsupported msg not possible - test pass.\n");
    }
  ck_assert_int_eq (true, thrown);
  thrown = false;
  try
    {
      Test_fake_dump fake (con);
    }
  catch (const Msg_not_available_exception &)
    {
      thrown = true;
      printf ("Constructing unsupported dump not possible - test pass.\n");
    }
  ck_assert_int_eq (true, thrown);
  thrown = false;
  try
    {
      Event_registration<Test_fake_details> fake (con);
    }
  catch (const Msg_not_available_exception &)
    {
      thrown = true;
      printf ("Constructing unsupported event registration not possible - "
              "test pass.\n");
    }
  ck_assert_int_eq (true, thrown);
}

END_TEST;

START_TEST (test_pmtu)
{
  printf ("--- Set ip_path_mtu to test stream rpc ---\n");
  const auto num_path_mtus = 5;
  { // new context
    for (int i = 0; i < num_path_mtus; ++i)
      {
	Ip_path_mtu_update d (con);
	auto &req = d.get_request ().get_payload ();
	req.pmtu.path_mtu = 1420;
	req.pmtu.nh.af = vapi_enum_address_family::ADDRESS_IP4;
	req.pmtu.nh.un.ip4[0] = 10;
	req.pmtu.nh.un.ip4[1] = 0;
	req.pmtu.nh.un.ip4[2] = 0;
	req.pmtu.nh.un.ip4[3] = i;
	auto rv = d.execute ();
	WAIT_FOR_RESPONSE (d, rv);
	ck_assert_int_eq (VAPI_OK, rv);
      }
  }

  { // new context
    bool seen[num_path_mtus] = { 0 };
    Ip_path_mtu_get d (con);
    d.get_request ().get_payload ().cursor = 0;
    auto rv = d.execute ();
    ck_assert_int_eq (VAPI_OK, rv);
    WAIT_FOR_RESPONSE (d, rv);
    ck_assert_int_eq (VAPI_OK, rv);
    auto &rs = d.get_result_set ();
    for (auto &r : rs)
      {
	auto &p = r.get_payload ();
	printf ("ip_path_mtu_get: mtu %hu ip %d.%d.%d.%d\n", p.pmtu.path_mtu,
		p.pmtu.nh.un.ip4[0], p.pmtu.nh.un.ip4[1], p.pmtu.nh.un.ip4[2],
		p.pmtu.nh.un.ip4[3]);
	for (int i = 0; i < num_path_mtus; ++i)
	  {
	    if (i == p.pmtu.nh.un.ip4[3])
	      {
		ck_assert_int_eq (0, seen[i]);
		seen[i] = true;
	      }
	  }
      }
    for (int i = 0; i < num_path_mtus; ++i)
      {
	ck_assert_int_eq (1, seen[i]);
      }
  }
}

END_TEST;

Suite *test_suite (void)
{
  Suite *s = suite_create ("VAPI test");

  TCase *tc_cpp_api = tcase_create ("C++ API");
  tcase_set_timeout (tc_cpp_api, 25);
  tcase_add_checked_fixture (tc_cpp_api, setup, teardown);
  tcase_add_test (tc_cpp_api, test_show_version_1);
  tcase_add_test (tc_cpp_api, test_show_version_2);
  tcase_add_test (tc_cpp_api, test_loopbacks_1);
  tcase_add_test (tc_cpp_api, test_loopbacks_2);
  tcase_add_test (tc_cpp_api, test_unsupported);
  tcase_add_test (tc_cpp_api, test_pmtu);
  suite_add_tcase (s, tc_cpp_api);

  return s;
}

int main (int argc, char *argv[])
{
  if (4 != argc)
    {
      printf ("Invalid argc==`%d'\n", argc);
      return EXIT_FAILURE;
    }
  app_name = argv[1];
  api_prefix = argv[2];
  if (!strcmp (argv[3], "shm"))
    use_uds = 0;
  else if (!strcmp (argv[3], "uds"))
    use_uds = 1;
  else
    {
      printf ("Unrecognised required argument '%s', expected 'uds' or 'shm'.",
	      argv[3]);
      return EXIT_FAILURE;
    }
  printf ("App name: `%s', API prefix: `%s', use unix sockets %d\n", app_name,
	  api_prefix, use_uds);

  int number_failed;
  Suite *s;
  SRunner *sr;

  s = test_suite ();
  sr = srunner_create (s);

  srunner_run_all (sr, CK_NORMAL);
  number_failed = srunner_ntests_failed (sr);
  srunner_free (sr);
  return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
