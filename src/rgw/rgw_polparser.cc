// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "include/buffer.h"

#include "common/ceph_argparse.h"
#include "common/common_init.h"

#include "global/global_init.h"

#include "rgw/rgw_iam_policy.h"

// Returns true on success
bool parse(CephContext* cct, const std::string& tenant,
           const std::string& fname, std::istream& in) noexcept
{
  bufferlist bl;
  bl.append(in);
  try {
    auto p = rgw::IAM::Policy(
      cct, tenant, bl,
      cct->_conf.get_val<bool>("rgw_policy_reject_invalid_principals"));
  } catch (const rgw::IAM::PolicyParseException& e) {
    std::cerr << fname << ": " << e.what() << std::endl;
    return false;
  } catch (const std::exception& e) {
    std::cerr << fname << ": caught exception: " << e.what() << std::endl;;
    return false;
  }
  return true;
}

void helpful_exit(std::string_view cmdname)
{
  std::cerr << cmdname << "-h for usage" << std::endl;
  exit(1);
}

void usage(std::string_view cmdname)
{
  std::cout << "usage: " << cmdname << " -t <tenant> [filename]"
	    << std::endl;
}

int main(int argc, const char** argv)
{
  std::string_view cmdname = argv[0];
  std::string tenant;

  auto args = argv_to_vec(argc, argv);
  if (ceph_argparse_need_usage(args)) {
    usage(cmdname);
    exit(0);
  }

  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY,
			 CINIT_FLAG_NO_DAEMON_ACTIONS |
			 CINIT_FLAG_NO_MON_CONFIG);
  common_init_finish(cct.get());
  std::string val;
  for (std::vector<const char*>::iterator i = args.begin(); i != args.end(); ) {
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_witharg(args, i, &val, "--tenant", "-t",
				     (char*)nullptr)) {
      tenant = std::move(val);
    } else {
      ++i;
    }
  }

  if (tenant.empty()) {
    std::cerr << cmdname << ": must specify tenant name" << std::endl;
    helpful_exit(cmdname);
  }

  bool success = true;

  if (args.empty()) {
    success = parse(cct.get(), tenant, "(stdin)", std::cin);
  } else {
    for (const auto& file : args) {
      std::ifstream in;
      in.open(file, std::ifstream::in);
      if (!in.is_open()) {
	std::cerr << "Can't read " << file << std::endl;
	success = false;
      }
      if (!parse(cct.get(), tenant, file, in)) {
	success = false;
      }
    }
  }

  return success ? 0 : 1;
}
