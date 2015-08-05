/* PROTO Manager */

#include "sql_class.h"
#include "proto_manager.h"

#include "google/protobuf/compiler/parser.h"
#include "google/protobuf/text_format.h"
#include "google/protobuf/dynamic_message.h"
#include "google/protobuf/io/tokenizer.h"
#include "google/protobuf/compiler/parser.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/message.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

#include <map>
#include <string>
#include <sstream>

using namespace google::protobuf;

bool Proto_manager::proto_is_valid(String *str)
{
  DescriptorPool pool;
  String desc_name;
  desc_name.append("doesnt_matter");

  /**
   * We use the temporary DescriptorPool 'pool' here, because we don't
   * know the table name yet, which means we also don't know which pool
   * to use from pool_map.
   */
  if (create_descriptor(str, &pool, &desc_name) != NULL)
      return true;
  return false;
}

// Parameter 'field_path' should be something like TABLE.COLUMN.
Descriptor *Proto_manager::get_descriptor(String *field_path, String *str)
{
  std::map<std::string, Descriptor*>::iterator it=
    descriptor_map.find(field_path->c_ptr());

  if (it != descriptor_map.end())
    return it->second;

  DescriptorPool *pool= new DescriptorPool;
  const Descriptor *mdesc= create_descriptor(str, pool, field_path);
  pool_map[field_path->c_ptr()]= pool;

  if (mdesc != NULL)
    descriptor_map[field_path->c_ptr()]= (Descriptor *)mdesc;
  return (Descriptor *)mdesc;
}

// This extracts the name of the message defined in a proto string.
// TODO(fanton): Make this more robust, accept multiple spaces and fix
// corner cases.
bool extract_message_name(String *str, char *message_name)
{
  int idx1, idx2;
  char const *ptr= str->c_ptr();

  DBUG_ASSERT(str && message_name);
  idx1= idx2= -1;

  for (uint32 i= 0; i < str->length(); ++i) {
    if (ptr[i] == ' ') {
      if (idx1 == -1) {
        idx1= i;
      } else {
        idx2= i;
        break;
      }
    }
  }

  if (idx1 != -1 && idx2 != -1) {
    if (idx2 - idx1 > MAX_MESSAGE_NAME_LENGTH) {
      DBUG_PRINT("error", ("Message name was too large."));
      return true;
    }
    memcpy(message_name, str->c_ptr() + idx1 + 1, idx2 - idx1 - 1);
    message_name[idx2 - idx1 - 1]= 0; // string terminator
    return false;
  }
  return true;
}

const Descriptor *Proto_manager::create_descriptor(String *str,
                                                   DescriptorPool *some_pool,
                                                   String *descr_name)
{
  compiler::Parser parser;
  FileDescriptorProto fdproto;
  char message_name[MAX_MESSAGE_NAME_LENGTH];

  std::stringstream ss(std::string(str->c_ptr(), str->c_ptr() + str->length()));
  io::IstreamInputStream iis(&ss, -1);
  io::Tokenizer tokenizer(&iis, &err_collector);

  if (!parser.Parse(&tokenizer, &fdproto))
  {
    DBUG_PRINT("error", ("Error parsing protobuf"));
    return NULL;
  }

  // We have to set a name for the descriptor, otherwise the protobuf
  // library will complain.
  if (!fdproto.has_name()) {
    if (extract_message_name(str, message_name))
      return NULL;

    String msg_file_name;
    msg_file_name.append(descr_name->c_ptr(), descr_name->length());
    msg_file_name.append(".proto");

    DBUG_PRINT("info", ("Setting file name to '%s'", msg_file_name.c_ptr()));
    fdproto.set_name(msg_file_name.c_ptr());
  }

  const FileDescriptor *fdesc= some_pool->BuildFile(fdproto);

  if (fdesc == NULL)
  {
    DBUG_PRINT("error", ("Error building file"));
    return NULL;
  }
  return fdesc->FindMessageTypeByName(message_name);
}

bool Proto_manager::encode(String *field_path, String *message,
                           String *proto_def, String *output)
{
  Message *mutable_msg= construct_message(field_path, proto_def);

  DBUG_ASSERT(mutable_msg);

  std::stringstream ss(std::string(message->c_ptr(),
                                   message->c_ptr() + message->length()));
  io::IstreamInputStream iis(&ss, -1);
  if (TextFormat::Parse(&iis, mutable_msg) == false)
  {
    DBUG_PRINT("error", ("Text format parse error."));
    delete mutable_msg;
    return true;
  }
  std::stringstream oss;
  mutable_msg->SerializeToOstream(&oss);
  output->length(0);
  output->append(oss.str().c_str(), oss.str().length());

  delete mutable_msg;
  return false;
}

bool Proto_manager::decode(String *field_path, String *message,
                           String *proto_def, String *output)
{
  Message *mutable_msg= construct_message(field_path, proto_def);

  DBUG_ASSERT(mutable_msg);

  std::stringstream ss(std::string(message->c_ptr(),
                                   message->c_ptr() + message->length()));
  if (mutable_msg->ParseFromIstream(&ss) == false)
  {
    DBUG_PRINT("error", ("Binary message parsing error."));
    return true;
  }

  std::string out_str;
  io::StringOutputStream sos(&out_str);
  TextFormat::Print(*mutable_msg, &sos);
  output->length(0);
  output->append(out_str.c_str(), out_str.length());

  return false;
}

Message *Proto_manager::construct_message(String *field_path, String *proto_def)
{
  Descriptor *proto_descr= NULL;
  proto_descr= get_descriptor(field_path, proto_def);

  if (proto_descr == NULL)
  {
    DBUG_PRINT("error", ("Proto descriptor was NULL"));
    return NULL;
  }
  const Message *proto_msg= factory.GetPrototype(proto_descr);
  DBUG_ASSERT(proto_msg);
  return proto_msg->New();
}
