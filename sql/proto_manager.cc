/* PROTO Manager */

#include "sql_class.h"
#include "parse_file.h"
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

const LEX_STRING proto_file_type= { C_STRING_WITH_LEN("PROTOS") };

struct proto_file_data
{
  List<LEX_STRING> definitions;
  List<LEX_STRING> fields;
};

static File_option file_params[] =
{
  {
    { C_STRING_WITH_LEN("protodefs") },
    my_offsetof(struct proto_file_data, definitions),
    FILE_OPTIONS_STRLIST
  },
  {
    { C_STRING_WITH_LEN("fields") },
    my_offsetof(struct proto_file_data, fields),
    FILE_OPTIONS_STRLIST
  },
  { {0, 0}, 0, FILE_OPTIONS_STRING }
};

static bool read_proto_file(struct proto_file_data *data,
                            LEX_STRING *file_name, MEM_ROOT *mem_root)
{
  File_parser *parser= sql_parse_prepare(file_name, mem_root, true);

  if (!parser) {
    DBUG_PRINT("error", ("Error getting parser"));
    return false;
  }

  parser->parse((uchar*)data, mem_root, file_params, 3, NULL);
  return true;
}

static bool append_to_proto_file(String *field_path, String *field_name,
                                 String *str, MEM_ROOT *mem_root)
{
  struct proto_file_data data;

  field_path->append(PROTO_FILE_EXTENSION);
  LEX_STRING file_name = field_path->lex_string();
  LEX_STRING proto_def = str->lex_string();
  LEX_STRING field = field_name->lex_string();

  if (!access(file_name.str, F_OK))
    read_proto_file(&data, &file_name, mem_root);

  data.definitions.push_back(&proto_def);
  data.fields.push_back(&field);

  if (sql_create_definition_file(NULL, &file_name, &proto_file_type,
                                 (uchar*)&data, file_params))
    return false;
  return true;
}

static bool find_proto_definition(struct proto_file_data *data, String *field,
                                  String *output)
{
  List_iterator_fast<LEX_STRING> it_definition(data->definitions);
  List_iterator_fast<LEX_STRING> it_field(data->fields);

  while (true) {
    LEX_STRING *definition = it_definition++;
    LEX_STRING *fld = it_field++;

    String fld_string;
    fld_string.append(fld);

    if (!definition || !fld)
      break;

    if (!stringcmp(field, &fld_string)) {
      output->length(0);
      output->append(definition);
      return true;
    }
  }

  return false;
}

bool Proto_manager::proto_is_valid(String *str, String *field_path,
                                   String *field_name, MEM_ROOT *mem_root)
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
      return append_to_proto_file(field_path, field_name, str, mem_root);
  return false;
}

// Parameter file_path should be something like DATABASE/TABLE.
Descriptor *Proto_manager::get_descriptor(String *file_path, String *field,
                                          MEM_ROOT *mem_root)
{
  String field_path;
  field_path.append(file_path->c_ptr(), file_path->length());
  field_path.append(field->c_ptr(), field->length());

  std::map<std::string, Descriptor*>::iterator it =
    descriptor_map.find(field_path.c_ptr());

  if (it != descriptor_map.end())
    return it->second;

  file_path->append(PROTO_FILE_EXTENSION);

  LEX_STRING file_name= file_path->lex_string();
  struct proto_file_data data;
  String proto_def;

  DBUG_ASSERT(read_proto_file(&data, &file_name, mem_root));
  DBUG_ASSERT(find_proto_definition(&data, field, &proto_def));

  DescriptorPool *pool= new DescriptorPool;
  const Descriptor *mdesc= create_descriptor(&proto_def, pool, &field_path);
  pool_map[field_path.c_ptr()]= pool;

  if (mdesc != NULL)
    descriptor_map[field_path.c_ptr()]= (Descriptor *)mdesc;
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

bool Proto_manager::encode(String *file_path, String *field, String *message,
                           MEM_ROOT *mem_root, String *output)
{
  Message *mutable_msg;

  mutable_msg= construct_message(file_path, field, mem_root);
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

bool Proto_manager::decode(String *file_path, String *field, String *message,
                           MEM_ROOT *mem_root, String *output)
{
  Proto_wrapper wr;
  if (construct_wrapper(file_path, field, message, mem_root, &wr))
  {
    DBUG_PRINT("error", ("Error constructing wrapper."));
    return true;
  }

  wr.to_text(output);
  return false;
}

Message *Proto_manager::construct_message(String *file_path, String *field,
                                          MEM_ROOT *mem_root)
{
  Descriptor *proto_descr= NULL;
  proto_descr= get_descriptor(file_path, field, mem_root);

  if (proto_descr == NULL)
  {
    DBUG_PRINT("error", ("Proto descriptor was NULL"));
    return NULL;
  }
  const Message *proto_msg= factory.GetPrototype(proto_descr);
  DBUG_ASSERT(proto_msg);
  return proto_msg->New();
}

bool Proto_manager::construct_wrapper(String *file_path, String *field,
                                      String *message, MEM_ROOT *mem_root,
                                      Proto_wrapper *wr)
{
  Message *mutable_msg = construct_message(file_path, field, mem_root);
  DBUG_ASSERT(mutable_msg);

  std::stringstream ss(std::string(message->c_ptr(),
                                   message->c_ptr() + message->length()));
  if (mutable_msg->ParseFromIstream(&ss) == false)
  {
    DBUG_PRINT("error", ("Binary message parsing error."));
    return true;
  }

  wr->setMessage(mutable_msg);
  return false;
}

bool Proto_wrapper::extract(String *field)
{
  const Reflection *refl= message->GetReflection();
  DBUG_ASSERT(refl);
  std::vector<const FieldDescriptor *> fdescs;
  bool found_field= false;

  refl->ListFields(*message, &fdescs);
  for (uint32 i= 0; i < fdescs.size(); ++i)
  {
    const FieldDescriptor *fdesc= fdescs[i];
    std::string field_name(field->c_ptr());

    if (!(fdesc->name().compare(field_name) == 0))
      refl->ClearField(message, fdesc);
    else
    {
      found_field= true;
      if (fdesc->type() == FieldDescriptor::TYPE_MESSAGE)
        return absorb_message(refl, fdesc);
    }
  }

  if (!found_field)
    return false;
  return true;
}

bool Proto_wrapper::to_text(String *val_ptr)
{
  std::string out_str;
  io::StringOutputStream sos(&out_str);
  DBUG_ASSERT(message || is_null);
  bool ret = true;

  val_ptr->length(0);
  if (!is_null)
  {
    if (number_of_fields() > 1)
    {
      TextFormat::Print(*message, &sos);
      std::replace(out_str.begin(), out_str.end(), '\n', ' ');
      val_ptr->append(out_str.c_str(), out_str.length());
    }
    else
    {
      ret = to_text_only_vals(val_ptr);
    }
  }
  else
  {
    // TODO(fanton): will fix this in a future commit. We should find a
    // way to propagate this upwards as a NULL value (probably by
    // changing the function parameter to be a Proto_wrapper and do the
    // checks in the caller.
    val_ptr->append("NULL");
  }

  return ret;
}

bool Proto_wrapper::absorb_message(const Reflection *refl,
                                   const FieldDescriptor *fdesc)
{
  const Message& msg = refl->GetMessage(*message, fdesc);
  message = (Message *)&msg;
  return true;
}

uint32 Proto_wrapper::number_of_fields()
{
  const Reflection *refl = message->GetReflection();
  DBUG_ASSERT(refl);
  std::vector<const FieldDescriptor *> fdescs;

  refl->ListFields(*message, &fdescs);
  return fdescs.size();
}

double Proto_wrapper::val_real()
{
  const Reflection *refl = message->GetReflection();
  DBUG_ASSERT(refl);
  std::vector<const FieldDescriptor *> fdescs;

  refl->ListFields(*message, &fdescs);

  if (fdescs.size() > 1)
    return 0.0;

  switch (fdescs[0]->type())
  {
    case FieldDescriptor::TYPE_DOUBLE:
      return refl->GetDouble(*message, fdescs[0]);
    case FieldDescriptor::TYPE_FLOAT:
      return (double)refl->GetFloat(*message, fdescs[0]);
    case FieldDescriptor::TYPE_INT32:
      return (double)refl->GetInt32(*message, fdescs[0]);
    case FieldDescriptor::TYPE_INT64:
      return (double)refl->GetInt64(*message, fdescs[0]);
    case FieldDescriptor::TYPE_UINT32:
      return (double)refl->GetUInt32(*message, fdescs[0]);
    case FieldDescriptor::TYPE_UINT64:
      return (double)refl->GetUInt64(*message, fdescs[0]);
    case FieldDescriptor::TYPE_STRING:
    {
      char *end_not_used;
      int not_used;

      string str = refl->GetString(*message, fdescs[0]);
      return my_strntod(&my_charset_utf8_bin, &str[0], str.size(), &end_not_used, &not_used);
    }
    default:
      return 0.0;
  }
  return 0.0;
}

// This method CAN be called for multiple fields, but is best called on
// a wrapper for a proto with only one field, otherwise may create
// confusion. Its output is also not compliant with the protobuf text
// syntax.
bool Proto_wrapper::to_text_only_vals(String *val_ptr)
{
  const Reflection *refl = message->GetReflection();
  DBUG_ASSERT(refl);
  std::vector<const FieldDescriptor *> fdescs;

  refl->ListFields(*message, &fdescs);
  val_ptr->length(0);
  for (uint32 i= 0; i< fdescs.size(); ++i)
  {
    const FieldDescriptor *fdesc = fdescs[i];
    std::string out_str;
    TextFormat::PrintFieldValueToString(*message, fdesc, -1, &out_str);
    val_ptr->append(out_str.c_str());
    val_ptr->append(" ");
  }

  // If we did at least one iteration, delete the trailing whitespace.
  if (val_ptr->length() > 0)
    val_ptr->chop();
  return true;
}

// Write the proto definition to "output".
bool Proto_manager::get_definition(String *file_path, String *field,
                                   MEM_ROOT *mem_root, String *output)
{
  file_path->append(PROTO_FILE_EXTENSION);

  LEX_STRING file_name = file_path->lex_string();
  struct proto_file_data data;

  DBUG_ASSERT(read_proto_file(&data, &file_name, mem_root));
  DBUG_ASSERT(find_proto_definition(&data, field, output));

  return false;
}

