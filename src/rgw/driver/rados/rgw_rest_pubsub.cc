// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <algorithm>
#include <boost/tokenizer.hpp>
#include <optional>
#include "rgw_rest_pubsub.h"
#include "rgw_pubsub_push.h"
#include "rgw_pubsub.h"
#include "rgw_op.h"
#include "rgw_rest.h"
#include "rgw_rest_s3.h"
#include "rgw_arn.h"
#include "rgw_auth_s3.h"
#include "rgw_notify.h"
#include "rgw_sal_rados.h"
#include "services/svc_zone.h"
#include "common/dout.h"
#include "rgw_url.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_rgw

static const char* AWS_SNS_NS("https://sns.amazonaws.com/doc/2010-03-31/");

bool verify_transport_security(CephContext *cct, const RGWEnv& env) {
  const auto is_secure = rgw_transport_is_secure(cct, env);
  if (!is_secure && g_conf().get_val<bool>("rgw_allow_notification_secrets_in_cleartext")) {
    ldout(cct, 0) << "WARNING: bypassing endpoint validation, allows sending secrets over insecure transport" << dendl;
    return true;
  }
  return is_secure;
}

// make sure that endpoint is a valid URL
// make sure that if user/password are passed inside URL, it is over secure connection
// update rgw_pubsub_dest to indicate that a password is stored in the URL
bool validate_and_update_endpoint_secret(rgw_pubsub_dest& dest, CephContext *cct, const RGWEnv& env) {
  if (dest.push_endpoint.empty()) {
      return true;
  }
  std::string user;
  std::string password;
  if (!rgw::parse_url_userinfo(dest.push_endpoint, user, password)) {
    ldout(cct, 1) << "endpoint validation error: malformed endpoint URL:" << dest.push_endpoint << dendl;
    return false;
  }
  // this should be verified inside parse_url()
  ceph_assert(user.empty() == password.empty());
  if (!user.empty()) {
      dest.stored_secret = true;
      if (!verify_transport_security(cct, env)) {
        ldout(cct, 1) << "endpoint validation error: sending secrets over insecure transport" << dendl;
        return false;
      }
  }
  return true;
}

bool topic_has_endpoint_secret(const rgw_pubsub_topic& topic) {
    return topic.dest.stored_secret;
}

bool topics_has_endpoint_secret(const rgw_pubsub_topics& topics) {
    for (const auto& topic : topics.topics) {
        if (topic_has_endpoint_secret(topic.second)) return true;
    }
    return false;
}

// command (AWS compliant): 
// POST
// Action=CreateTopic&Name=<topic-name>[&OpaqueData=data][&push-endpoint=<endpoint>[&persistent][&<arg1>=<value1>]]
class RGWPSCreateTopicOp : public RGWOp {
  private:
  std::string topic_name;
  rgw_pubsub_dest dest;
  std::string topic_arn;
  std::string opaque_data;
  
  int get_params() {
    topic_name = s->info.args.get("Name");
    if (topic_name.empty()) {
      ldpp_dout(this, 1) << "CreateTopic Action 'Name' argument is missing" << dendl;
      return -EINVAL;
    }

    opaque_data = s->info.args.get("OpaqueData");

    dest.push_endpoint = s->info.args.get("push-endpoint");
    s->info.args.get_bool("persistent", &dest.persistent, false);

    if (!validate_and_update_endpoint_secret(dest, s->cct, *(s->info.env))) {
      return -EINVAL;
    }
    for (const auto& param : s->info.args.get_params()) {
      if (param.first == "Action" || param.first == "Name" || param.first == "PayloadHash") {
        continue;
      }
      dest.push_endpoint_args.append(param.first+"="+param.second+"&");
    }

    if (!dest.push_endpoint_args.empty()) {
      // remove last separator
      dest.push_endpoint_args.pop_back();
    }
    if (!dest.push_endpoint.empty() && dest.persistent) {
      const auto ret = rgw::notify::add_persistent_topic(topic_name, s->yield);
      if (ret < 0) {
        ldpp_dout(this, 1) << "CreateTopic Action failed to create queue for persistent topics. error:" << ret << dendl;
        return ret;
      }
    }
    
    // dest object only stores endpoint info
    dest.arn_topic = topic_name;
    // the topic ARN will be sent in the reply
    const rgw::ARN arn(rgw::Partition::aws, rgw::Service::sns, 
        driver->get_zone()->get_zonegroup().get_name(),
        s->user->get_tenant(), topic_name);
    topic_arn = arn.to_string();
    return 0;
  }

  public:
  int verify_permission(optional_yield) override {
    return 0;
  }

  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }
  void execute(optional_yield) override;

  const char* name() const override { return "pubsub_topic_create"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_TOPIC_CREATE; }
  uint32_t op_mask() override { return RGW_OP_TYPE_WRITE; }

  void send_response() override {
    if (op_ret) {
      set_req_state_err(s, op_ret);
    }
    dump_errno(s);
    end_header(s, this, "application/xml");

    if (op_ret < 0) {
      return;
    }

    const auto f = s->formatter;
    f->open_object_section_in_ns("CreateTopicResponse", AWS_SNS_NS);
    f->open_object_section("CreateTopicResult");
    encode_xml("TopicArn", topic_arn, f); 
    f->close_section(); // CreateTopicResult
    f->open_object_section("ResponseMetadata");
    encode_xml("RequestId", s->req_id, f); 
    f->close_section(); // ResponseMetadata
    f->close_section(); // CreateTopicResponse
    rgw_flush_formatter_and_reset(s, f);
  }
};

void RGWPSCreateTopicOp::execute(optional_yield y) {
  op_ret = get_params();
  if (op_ret < 0) {
    return;
  }

  RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  op_ret = ps.create_topic(this, topic_name, dest, topic_arn, opaque_data, y);
  if (op_ret < 0) {
    ldpp_dout(this, 1) << "failed to create topic '" << topic_name << "', ret=" << op_ret << dendl;
    return;
  }
  ldpp_dout(this, 20) << "successfully created topic '" << topic_name << "'" << dendl;
}

// command (AWS compliant): 
// POST 
// Action=ListTopics
class RGWPSListTopicsOp : public RGWOp {
private:
  rgw_pubsub_topics result;

public:
  int verify_permission(optional_yield) override {
    return 0;
  }
  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }
  void execute(optional_yield) override;

  const char* name() const override { return "pubsub_topics_list"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_TOPICS_LIST; }
  uint32_t op_mask() override { return RGW_OP_TYPE_READ; }

  void send_response() override {
    if (op_ret) {
      set_req_state_err(s, op_ret);
    }
    dump_errno(s);
    end_header(s, this, "application/xml");

    if (op_ret < 0) {
      return;
    }

    const auto f = s->formatter;
    f->open_object_section_in_ns("ListTopicsResponse", AWS_SNS_NS);
    f->open_object_section("ListTopicsResult");
    encode_xml("Topics", result, f); 
    f->close_section(); // ListTopicsResult
    f->open_object_section("ResponseMetadata");
    encode_xml("RequestId", s->req_id, f); 
    f->close_section(); // ResponseMetadat
    f->close_section(); // ListTopicsResponse
    rgw_flush_formatter_and_reset(s, f);
  }
};

void RGWPSListTopicsOp::execute(optional_yield y) {
  RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  op_ret = ps.get_topics(&result);
  // if there are no topics it is not considered an error
  op_ret = op_ret == -ENOENT ? 0 : op_ret;
  if (op_ret < 0) {
    ldpp_dout(this, 1) << "failed to get topics, ret=" << op_ret << dendl;
    return;
  }
  if (topics_has_endpoint_secret(result) && !verify_transport_security(s->cct, *(s->info.env))) {
    ldpp_dout(this, 1) << "topics contain secrets and cannot be sent over insecure transport" << dendl;
    op_ret = -EPERM;
    return;
  }
  ldpp_dout(this, 20) << "successfully got topics" << dendl;
}

// command (extension to AWS): 
// POST
// Action=GetTopic&TopicArn=<topic-arn>
class RGWPSGetTopicOp : public RGWOp {
  private:
  std::string topic_name;
  rgw_pubsub_topic result;
  
  int get_params() {
    const auto topic_arn = rgw::ARN::parse((s->info.args.get("TopicArn")));

    if (!topic_arn || topic_arn->resource.empty()) {
        ldpp_dout(this, 1) << "GetTopic Action 'TopicArn' argument is missing or invalid" << dendl;
        return -EINVAL;
    }

    topic_name = topic_arn->resource;
    return 0;
  }

  public:
  int verify_permission(optional_yield y) override {
    return 0;
  }
  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }
  void execute(optional_yield y) override;

  const char* name() const override { return "pubsub_topic_get"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_TOPIC_GET; }
  uint32_t op_mask() override { return RGW_OP_TYPE_READ; }

  void send_response() override {
    if (op_ret) {
      set_req_state_err(s, op_ret);
    }
    dump_errno(s);
    end_header(s, this, "application/xml");

    if (op_ret < 0) {
      return;
    }

    const auto f = s->formatter;
    f->open_object_section("GetTopicResponse");
    f->open_object_section("GetTopicResult");
    encode_xml("Topic", result, f); 
    f->close_section();
    f->open_object_section("ResponseMetadata");
    encode_xml("RequestId", s->req_id, f); 
    f->close_section();
    f->close_section();
    rgw_flush_formatter_and_reset(s, f);
  }
};

void RGWPSGetTopicOp::execute(optional_yield y) {
  op_ret = get_params();
  if (op_ret < 0) {
    return;
  }
  RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  op_ret = ps.get_topic(topic_name, &result);
  if (op_ret < 0) {
    ldpp_dout(this, 1) << "failed to get topic '" << topic_name << "', ret=" << op_ret << dendl;
    return;
  }
  if (topic_has_endpoint_secret(result) && !verify_transport_security(s->cct, *(s->info.env))) {
    ldpp_dout(this, 1) << "topic '" << topic_name << "' contain secret and cannot be sent over insecure transport" << dendl;
    op_ret = -EPERM;
    return;
  }
  ldpp_dout(this, 1) << "successfully got topic '" << topic_name << "'" << dendl;
}

// command (AWS compliant): 
// POST
// Action=GetTopicAttributes&TopicArn=<topic-arn>
class RGWPSGetTopicAttributesOp : public RGWOp {
  private:
  std::string topic_name;
  rgw_pubsub_topic result;
  
  int get_params() {
    const auto topic_arn = rgw::ARN::parse((s->info.args.get("TopicArn")));

    if (!topic_arn || topic_arn->resource.empty()) {
        ldpp_dout(this, 1) << "GetTopicAttribute Action 'TopicArn' argument is missing or invalid" << dendl;
        return -EINVAL;
    }

    topic_name = topic_arn->resource;
    return 0;
  }

  public:
  int verify_permission(optional_yield y) override {
    return 0;
  }
  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }
  void execute(optional_yield y) override;

  const char* name() const override { return "pubsub_topic_get"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_TOPIC_GET; }
  uint32_t op_mask() override { return RGW_OP_TYPE_READ; }

  void send_response() override {
    if (op_ret) {
      set_req_state_err(s, op_ret);
    }
    dump_errno(s);
    end_header(s, this, "application/xml");

    if (op_ret < 0) {
      return;
    }

    const auto f = s->formatter;
    f->open_object_section_in_ns("GetTopicAttributesResponse", AWS_SNS_NS);
    f->open_object_section("GetTopicAttributesResult");
    result.dump_xml_as_attributes(f);
    f->close_section(); // GetTopicAttributesResult
    f->open_object_section("ResponseMetadata");
    encode_xml("RequestId", s->req_id, f); 
    f->close_section(); // ResponseMetadata
    f->close_section(); // GetTopicAttributesResponse
    rgw_flush_formatter_and_reset(s, f);
  }
};

void RGWPSGetTopicAttributesOp::execute(optional_yield y) {
  op_ret = get_params();
  if (op_ret < 0) {
    return;
  }
  RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  op_ret = ps.get_topic(topic_name, &result);
  if (op_ret < 0) {
    ldpp_dout(this, 1) << "failed to get topic '" << topic_name << "', ret=" << op_ret << dendl;
    return;
  }
  if (topic_has_endpoint_secret(result) && !verify_transport_security(s->cct, *(s->info.env))) {
    ldpp_dout(this, 1) << "topic '" << topic_name << "' contain secret and cannot be sent over insecure transport" << dendl;
    op_ret = -EPERM;
    return;
  }
  ldpp_dout(this, 1) << "successfully got topic '" << topic_name << "'" << dendl;
}

// command (AWS compliant): 
// POST
// Action=DeleteTopic&TopicArn=<topic-arn>
class RGWPSDeleteTopicOp : public RGWOp {
  private:
  std::string topic_name;
  
  int get_params() {
    const auto topic_arn = rgw::ARN::parse((s->info.args.get("TopicArn")));

    if (!topic_arn || topic_arn->resource.empty()) {
      ldpp_dout(this, 1) << "DeleteTopic Action 'TopicArn' argument is missing or invalid" << dendl;
      return -EINVAL;
    }

    topic_name = topic_arn->resource;

    // upon deletion it is not known if topic is persistent or not
    // will try to delete the persistent topic anyway
    const auto ret = rgw::notify::remove_persistent_topic(topic_name, s->yield);
    if (ret == -ENOENT) {
      // topic was not persistent, or already deleted
      return 0;
    }
    if (ret < 0) {
      ldpp_dout(this, 1) << "DeleteTopic Action failed to remove queue for persistent topics. error:" << ret << dendl;
      return ret;
    }

    return 0;
  }

  public:
  int verify_permission(optional_yield) override {
    return 0;
  }
  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }
  void execute(optional_yield y) override;

  const char* name() const override { return "pubsub_topic_delete"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_TOPIC_DELETE; }
  uint32_t op_mask() override { return RGW_OP_TYPE_DELETE; }

  void send_response() override {
    if (op_ret) {
      set_req_state_err(s, op_ret);
    }
    dump_errno(s);
    end_header(s, this, "application/xml");

    if (op_ret < 0) {
      return;
    }

    const auto f = s->formatter;
    f->open_object_section_in_ns("DeleteTopicResponse", AWS_SNS_NS);
    f->open_object_section("ResponseMetadata");
    encode_xml("RequestId", s->req_id, f); 
    f->close_section(); // ResponseMetadata
    f->close_section(); // DeleteTopicResponse
    rgw_flush_formatter_and_reset(s, f);
  }
};

void RGWPSDeleteTopicOp::execute(optional_yield y) {
  op_ret = get_params();
  if (op_ret < 0) {
    return;
  }
  RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  op_ret = ps.remove_topic(this, topic_name, y);
  if (op_ret < 0) {
    ldpp_dout(this, 1) << "failed to remove topic '" << topic_name << ", ret=" << op_ret << dendl;
    return;
  }
  ldpp_dout(this, 1) << "successfully removed topic '" << topic_name << "'" << dendl;
}

using op_generator = RGWOp*(*)();
static const std::unordered_map<std::string, op_generator> op_generators = {
  {"CreateTopic", []() -> RGWOp* {return new RGWPSCreateTopicOp;}},
  {"DeleteTopic", []() -> RGWOp* {return new RGWPSDeleteTopicOp;}},
  {"ListTopics", []() -> RGWOp* {return new RGWPSListTopicsOp;}},
  {"GetTopic", []() -> RGWOp* {return new RGWPSGetTopicOp;}},
  {"GetTopicAttributes", []() -> RGWOp* {return new RGWPSGetTopicAttributesOp;}}
};

bool RGWHandler_REST_PSTopic_AWS::action_exists(const req_state* s) 
{
  if (s->info.args.exists("Action")) {
    const std::string action_name = s->info.args.get("Action");
    return op_generators.contains(action_name);
  }
  return false;
}

RGWOp *RGWHandler_REST_PSTopic_AWS::op_post()
{
  s->dialect = "sns";
  s->prot_flags = RGW_REST_STS;

  if (s->info.args.exists("Action")) {
    const std::string action_name = s->info.args.get("Action");
    const auto action_it = op_generators.find(action_name);
    if (action_it != op_generators.end()) {
      return action_it->second();
    }
    ldpp_dout(s, 10) << "unknown action '" << action_name << "' for Topic handler" << dendl;
  } else {
    ldpp_dout(s, 10) << "missing action argument in Topic handler" << dendl;
  }
  return nullptr;
}

int RGWHandler_REST_PSTopic_AWS::authorize(const DoutPrefixProvider* dpp, optional_yield y) {
  const auto rc = RGW_Auth_S3::authorize(dpp, driver, auth_registry, s, y);
  if (rc < 0) {
    return rc;
  }
  if (s->auth.identity->is_anonymous()) {
    ldpp_dout(dpp, 1) << "anonymous user not allowed in topic operations" << dendl;
    return -ERR_INVALID_REQUEST;
  }
  return 0;
}

namespace {
// return a unique topic by prefexing with the notification name: <notification>_<topic>
std::string topic_to_unique(const std::string& topic, const std::string& notification) {
  return notification + "_" + topic;
}

// extract the topic from a unique topic of the form: <notification>_<topic>
[[maybe_unused]] std::string unique_to_topic(const std::string& unique_topic, const std::string& notification) {
  if (unique_topic.find(notification + "_") == std::string::npos) {
    return "";
  }
  return unique_topic.substr(notification.length() + 1);
}

// from list of bucket topics, find the one that was auto-generated by a notification
auto find_unique_topic(const rgw_pubsub_bucket_topics& bucket_topics, const std::string& notif_name) {
    auto it = std::find_if(bucket_topics.topics.begin(), bucket_topics.topics.end(), [&](const auto& val) { return notif_name == val.second.s3_id; });
    return it != bucket_topics.topics.end() ?
        std::optional<std::reference_wrapper<const rgw_pubsub_topic_filter>>(it->second):
        std::nullopt;
}
}

int remove_notification_by_topic(const DoutPrefixProvider *dpp, const std::string& topic_name, const RGWPubSub::Bucket& b, optional_yield y, const RGWPubSub& ps) {
  int op_ret = b.remove_notification(dpp, topic_name, y);
  if (op_ret < 0) {
    ldpp_dout(dpp, 1) << "failed to remove notification of topic '" << topic_name << "', ret=" << op_ret << dendl;
  }
  op_ret = ps.remove_topic(dpp, topic_name, y);
  if (op_ret < 0) {
    ldpp_dout(dpp, 1) << "failed to remove auto-generated topic '" << topic_name << "', ret=" << op_ret << dendl;
  }
  return op_ret;
}

int delete_all_notifications(const DoutPrefixProvider *dpp, const rgw_pubsub_bucket_topics& bucket_topics, const RGWPubSub::Bucket& b, optional_yield y, const RGWPubSub& ps) {
  // delete all notifications of on a bucket
  for (const auto& topic : bucket_topics.topics) {
    const auto op_ret = remove_notification_by_topic(dpp, topic.first, b, y, ps);
    if (op_ret < 0) {
      return op_ret;
    }
  }
  return 0;
}

// command (S3 compliant): PUT /<bucket name>?notification
// a "notification" and a subscription will be auto-generated
// actual configuration is XML encoded in the body of the message
class RGWPSCreateNotifOp : public RGWDefaultResponseOp {
  private:
  std::string bucket_name;
  RGWBucketInfo bucket_info;
  rgw_pubsub_s3_notifications configurations;

  int get_params() {
    bool exists;
    const auto no_value = s->info.args.get("notification", &exists);
    if (!exists) {
      ldpp_dout(this, 1) << "missing required param 'notification'" << dendl;
      return -EINVAL;
    } 
    if (no_value.length() > 0) {
      ldpp_dout(this, 1) << "param 'notification' should not have any value" << dendl;
      return -EINVAL;
    }
    if (s->bucket_name.empty()) {
      ldpp_dout(this, 1) << "request must be on a bucket" << dendl;
      return -EINVAL;
    }
    bucket_name = s->bucket_name;
    return 0;
  }

  public:
  int verify_permission(optional_yield y) override;

  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }

  const char* name() const override { return "pubsub_notification_create_s3"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_NOTIF_CREATE; }
  uint32_t op_mask() override { return RGW_OP_TYPE_WRITE; }

  int get_params_from_body() {
    const auto max_size = s->cct->_conf->rgw_max_put_param_size;
    int r;
    bufferlist data;
    std::tie(r, data) = read_all_input(s, max_size, false);

    if (r < 0) {
      ldpp_dout(this, 1) << "failed to read XML payload" << dendl;
      return r;
    }
    if (data.length() == 0) {
      ldpp_dout(this, 1) << "XML payload missing" << dendl;
      return -EINVAL;
    }

    RGWXMLDecoder::XMLParser parser;

    if (!parser.init()){
      ldpp_dout(this, 1) << "failed to initialize XML parser" << dendl;
      return -EINVAL;
    }
    if (!parser.parse(data.c_str(), data.length(), 1)) {
      ldpp_dout(this, 1) << "failed to parse XML payload" << dendl;
      return -ERR_MALFORMED_XML;
    }
    try {
      // NotificationConfigurations is mandatory
      // It can be empty which means we delete all the notifications
      RGWXMLDecoder::decode_xml("NotificationConfiguration", configurations, &parser, true);
    } catch (RGWXMLDecoder::err& err) {
      ldpp_dout(this, 1) << "failed to parse XML payload. error: " << err << dendl;
      return -ERR_MALFORMED_XML;
    }
    return 0;
  }

  void execute(optional_yield) override;
};

void RGWPSCreateNotifOp::execute(optional_yield y) {
  op_ret = get_params_from_body();
  if (op_ret < 0) {
    return;
  }

  const RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  const RGWPubSub::Bucket b(ps, bucket_info.bucket);

  if(configurations.list.empty()) {
    // get all topics on a bucket
    rgw_pubsub_bucket_topics bucket_topics;
    op_ret = b.get_topics(&bucket_topics);
    if (op_ret < 0) {
      ldpp_dout(this, 1) << "failed to get list of topics from bucket '" << bucket_info.bucket.name << "', ret=" << op_ret << dendl;
      return;
    }

    op_ret = delete_all_notifications(this, bucket_topics, b, y, ps);
    return;
  }

  for (const auto& c : configurations.list) {
    const auto& notif_name = c.id;
    if (notif_name.empty()) {
      ldpp_dout(this, 1) << "missing notification id" << dendl;
      op_ret = -EINVAL;
      return;
    }
    if (c.topic_arn.empty()) {
      ldpp_dout(this, 1) << "missing topic ARN in notification: '" << notif_name << "'" << dendl;
      op_ret = -EINVAL;
      return;
    }

    const auto arn = rgw::ARN::parse(c.topic_arn);
    if (!arn || arn->resource.empty()) {
      ldpp_dout(this, 1) << "topic ARN has invalid format: '" << c.topic_arn << "' in notification: '" << notif_name << "'" << dendl;
      op_ret = -EINVAL;
      return;
    }

    if (std::find(c.events.begin(), c.events.end(), rgw::notify::UnknownEvent) != c.events.end()) {
      ldpp_dout(this, 1) << "unknown event type in notification: '" << notif_name << "'" << dendl;
      op_ret = -EINVAL;
      return;
    }

    const auto topic_name = arn->resource;

    // get topic information. destination information is stored in the topic
    rgw_pubsub_topic topic_info;  
    op_ret = ps.get_topic(topic_name, &topic_info);
    if (op_ret < 0) {
      ldpp_dout(this, 1) << "failed to get topic '" << topic_name << "', ret=" << op_ret << dendl;
      return;
    }
    // make sure that full topic configuration match
    // TODO: use ARN match function
    
    // create unique topic name. this has 2 reasons:
    // (1) topics cannot be shared between different S3 notifications because they hold the filter information
    // (2) make topic clneaup easier, when notification is removed
    const auto unique_topic_name = topic_to_unique(topic_name, notif_name);
    // generate the internal topic. destination is stored here for the "push-only" case
    // when no subscription exists
    // ARN is cached to make the "GET" method faster
    op_ret = ps.create_topic(this, unique_topic_name, topic_info.dest, topic_info.arn, topic_info.opaque_data, y);
    if (op_ret < 0) {
      ldpp_dout(this, 1) << "failed to auto-generate unique topic '" << unique_topic_name << 
        "', ret=" << op_ret << dendl;
      return;
    }
    ldpp_dout(this, 20) << "successfully auto-generated unique topic '" << unique_topic_name << "'" << dendl;
    // generate the notification
    rgw::notify::EventTypeList events;
    op_ret = b.create_notification(this, unique_topic_name, c.events, std::make_optional(c.filter), notif_name, y);
    if (op_ret < 0) {
      ldpp_dout(this, 1) << "failed to auto-generate notification for unique topic '" << unique_topic_name <<
        "', ret=" << op_ret << dendl;
      // rollback generated topic (ignore return value)
      ps.remove_topic(this, unique_topic_name, y);
      return;
    }
    ldpp_dout(this, 20) << "successfully auto-generated notification for unique topic '" << unique_topic_name << "'" << dendl;
  }
}

int RGWPSCreateNotifOp::verify_permission(optional_yield y) {
  int ret = get_params();
  if (ret < 0) {
    return ret;
  }

  std::unique_ptr<rgw::sal::User> user = driver->get_user(s->owner.get_id());
  std::unique_ptr<rgw::sal::Bucket> bucket;
  ret = driver->get_bucket(this, user.get(), s->owner.get_id().tenant, bucket_name, &bucket, y);
  if (ret < 0) {
    ldpp_dout(this, 1) << "failed to get bucket info, cannot verify ownership" << dendl;
    return ret;
  }
  bucket_info = bucket->get_info();

  if (bucket_info.owner != s->owner.get_id()) {
    ldpp_dout(this, 1) << "user doesn't own bucket, not allowed to create notification" << dendl;
    return -EPERM;
  }
  return 0;
}

// command (extension to S3): DELETE /bucket?notification[=<notification-id>]
class RGWPSDeleteNotifOp : public RGWDefaultResponseOp {
  private:
  std::string bucket_name;
  RGWBucketInfo bucket_info;
  std::string notif_name;
  
  public:
  int verify_permission(optional_yield y) override;

  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }
  
  const char* name() const override { return "pubsub_notification_delete_s3"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_NOTIF_DELETE; }
  uint32_t op_mask() override { return RGW_OP_TYPE_DELETE; }

  int get_params() {
    bool exists;
    notif_name = s->info.args.get("notification", &exists);
    if (!exists) {
      ldpp_dout(this, 1) << "missing required param 'notification'" << dendl;
      return -EINVAL;
    } 
    if (s->bucket_name.empty()) {
      ldpp_dout(this, 1) << "request must be on a bucket" << dendl;
      return -EINVAL;
    }
    bucket_name = s->bucket_name;
    return 0;
  }

  void execute(optional_yield y) override;
};

void RGWPSDeleteNotifOp::execute(optional_yield y) {
  op_ret = get_params();
  if (op_ret < 0) {
    return;
  }

  const RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  const RGWPubSub::Bucket b(ps, bucket_info.bucket);

  // get all topics on a bucket
  rgw_pubsub_bucket_topics bucket_topics;
  op_ret = b.get_topics(&bucket_topics);
  if (op_ret < 0) {
    ldpp_dout(this, 1) << "failed to get list of topics from bucket '" << bucket_info.bucket.name << "', ret=" << op_ret << dendl;
    return;
  }

  if (!notif_name.empty()) {
    // delete a specific notification
    const auto unique_topic = find_unique_topic(bucket_topics, notif_name);
    if (unique_topic) {
      const auto unique_topic_name = unique_topic->get().topic.name;
      op_ret = remove_notification_by_topic(this, unique_topic_name, b, y, ps);
      return;
    }
    // notification to be removed is not found - considered success
    ldpp_dout(this, 20) << "notification '" << notif_name << "' already removed" << dendl;
    return;
  }

  op_ret = delete_all_notifications(this, bucket_topics, b, y, ps);
}

int RGWPSDeleteNotifOp::verify_permission(optional_yield y) {
  int ret = get_params();
  if (ret < 0) {
    return ret;
  }

  std::unique_ptr<rgw::sal::User> user = driver->get_user(s->owner.get_id());
  std::unique_ptr<rgw::sal::Bucket> bucket;
  ret = driver->get_bucket(this, user.get(), s->owner.get_id().tenant, bucket_name, &bucket, y);
  if (ret < 0) {
    return ret;
  }
  bucket_info = bucket->get_info();

  if (bucket_info.owner != s->owner.get_id()) {
    ldpp_dout(this, 1) << "user doesn't own bucket, cannot remove notification" << dendl;
    return -EPERM;
  }
  return 0;
}

// command (S3 compliant): GET /bucket?notification[=<notification-id>]
class RGWPSListNotifsOp : public RGWOp {
private:
  std::string bucket_name;
  RGWBucketInfo bucket_info;
  std::string notif_name;
  rgw_pubsub_s3_notifications notifications;

  int get_params() {
    bool exists;
    notif_name = s->info.args.get("notification", &exists);
    if (!exists) {
      ldpp_dout(this, 1) << "missing required param 'notification'" << dendl;
      return -EINVAL;
    } 
    if (s->bucket_name.empty()) {
      ldpp_dout(this, 1) << "request must be on a bucket" << dendl;
      return -EINVAL;
    }
    bucket_name = s->bucket_name;
    return 0;
  }

  public:
  int verify_permission(optional_yield y) override;

  void pre_exec() override {
    rgw_bucket_object_pre_exec(s);
  }

  const char* name() const override { return "pubsub_notifications_get_s3"; }
  RGWOpType get_type() override { return RGW_OP_PUBSUB_NOTIF_LIST; }
  uint32_t op_mask() override { return RGW_OP_TYPE_READ; }

  void execute(optional_yield y) override;
  void send_response() override {
    if (op_ret) {
      set_req_state_err(s, op_ret);
    }
    dump_errno(s);
    end_header(s, this, "application/xml");

    if (op_ret < 0) {
      return;
    }
    notifications.dump_xml(s->formatter);
    rgw_flush_formatter_and_reset(s, s->formatter);
  }
};

void RGWPSListNotifsOp::execute(optional_yield y) {
  const RGWPubSub ps(static_cast<rgw::sal::RadosStore*>(driver), s->owner.get_id().tenant);
  const RGWPubSub::Bucket b(ps, bucket_info.bucket);
  
  // get all topics on a bucket
  rgw_pubsub_bucket_topics bucket_topics;
  op_ret = b.get_topics(&bucket_topics);
  if (op_ret < 0) {
    ldpp_dout(this, 1) << "failed to get list of topics from bucket '" << bucket_info.bucket.name << "', ret=" << op_ret << dendl;
    return;
  }
  if (!notif_name.empty()) {
    // get info of a specific notification
    const auto unique_topic = find_unique_topic(bucket_topics, notif_name);
    if (unique_topic) {
      notifications.list.emplace_back(unique_topic->get());
      return;
    }
    op_ret = -ENOENT;
    ldpp_dout(this, 1) << "failed to get notification info for '" << notif_name << "', ret=" << op_ret << dendl;
    return;
  }
  // loop through all topics of the bucket
  for (const auto& topic : bucket_topics.topics) {
    if (topic.second.s3_id.empty()) {
        // not an s3 notification
        continue;
    }
    notifications.list.emplace_back(topic.second);
  }
}

int RGWPSListNotifsOp::verify_permission(optional_yield y) {
  int ret = get_params();
  if (ret < 0) {
    return ret;
  }

  std::unique_ptr<rgw::sal::User> user = driver->get_user(s->owner.get_id());
  std::unique_ptr<rgw::sal::Bucket> bucket;
  ret = driver->get_bucket(this, user.get(), s->owner.get_id().tenant, bucket_name, &bucket, y);
  if (ret < 0) {
    return ret;
  }
  bucket_info = bucket->get_info();

  if (bucket_info.owner != s->owner.get_id()) {
    ldpp_dout(this, 1) << "user doesn't own bucket, cannot get notification list" << dendl;
    return -EPERM;
  }

  return 0;
}

RGWOp* RGWHandler_REST_PSNotifs_S3::op_get() {
  return new RGWPSListNotifsOp();
}

RGWOp* RGWHandler_REST_PSNotifs_S3::op_put() {
  return new RGWPSCreateNotifOp();
}

RGWOp* RGWHandler_REST_PSNotifs_S3::op_delete() {
  return new RGWPSDeleteNotifOp();
}

RGWOp* RGWHandler_REST_PSNotifs_S3::create_get_op() {
    return new RGWPSListNotifsOp();
}

RGWOp* RGWHandler_REST_PSNotifs_S3::create_put_op() {
  return new RGWPSCreateNotifOp();
}

RGWOp* RGWHandler_REST_PSNotifs_S3::create_delete_op() {
  return new RGWPSDeleteNotifOp();
}

