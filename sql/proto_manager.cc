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

  bool success;
  success= read_proto_file(&data, &file_name, mem_root);
  DBUG_ASSERT(success);
  success= find_proto_definition(&data, field, &proto_def);
  DBUG_ASSERT(success);

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

bool Proto_manager::construct_wrapper_message(const Descriptor *desc,
                                              String *text, Proto_wrapper *wr)
{
  const Message *proto_msg= factory.GetPrototype(desc);
  DBUG_ASSERT(proto_msg);
  Message *mutable_msg = proto_msg->New();
  DBUG_ASSERT(mutable_msg);

  std::stringstream ss(std::string(text->c_ptr(),
                                   text->c_ptr() + text->length()));

  io::IstreamInputStream iis(&ss, -1);
  if (TextFormat::Parse(&iis, mutable_msg) == false)
  {
    DBUG_PRINT("error", ("Text parse error."));
    return true;
  }

  wr->setMessage(mutable_msg);
  return false;
}

bool Proto_manager::construct_wrapper_value(const Descriptor *desc, String *text,
                                            Proto_wrapper *wr,
                                            String *field)
{
  const Message *proto_msg= factory.GetPrototype(desc);
  DBUG_ASSERT(proto_msg);
  Message *mutable_msg = proto_msg->New();
  DBUG_ASSERT(mutable_msg);

  std::string field_name(field->c_ptr());
  const FieldDescriptor *fdesc = desc->FindFieldByName(field_name);

  if (!fdesc)
    return -1;

  std::string str(std::string(text->c_ptr(),
                                   text->c_ptr() + text->length()));

  /**
   * This is a workaround to allow the user to avoid quoting every
   * string. Unfortunately, we still have to quote it manually here,
   * if the user hasn't, otherwise the protobuf parser won't recognize
   * it.
   */
  if (fdesc->type() == FieldDescriptor::TYPE_STRING && str[0] != '"')
  {
      str.insert(0, "\"");
      str.append("\"");
  }

  if (TextFormat::ParseFieldValueFromString(str, fdesc,
                                            mutable_msg) == false)
  {
    DBUG_PRINT("error", ("Text parse error."));
    return true;
  }

  wr->setMessage(mutable_msg);
  if (fdesc->is_repeated())
    wr->setRepeatedIndex(0);

  return false;
}

bool Proto_wrapper::extract(String *field)
{
  // Check if the field is defined in the proto definition.
  const Descriptor *desc = message->GetDescriptor();
  std::string field_name(field->c_ptr());

  if (!desc->FindFieldByName(field_name))
    return true;

  const Reflection *refl= message->GetReflection();
  DBUG_ASSERT(refl);
  std::vector<const FieldDescriptor *> fdescs;
  bool found_field= false;

  refl->ListFields(*message, &fdescs);
  for (uint32 i= 0; i < fdescs.size(); ++i)
  {
    const FieldDescriptor *fdesc= fdescs[i];

    if (!(fdesc->name().compare(field_name) == 0))
      refl->ClearField(message, fdesc);
    else
    {
      found_field= true;
      if (fdesc->type() == FieldDescriptor::TYPE_MESSAGE)
        return absorb_message(refl, fdesc);
    }
  }

  if (found_field)
    setLeaf(); // this is definitely not a message, so it must be a leaf
  else
    setNull(); // we have the field in the definition, but we didn't find it

  return false;
}

/**
 * This function should be called only when we know that we have a
 * repeated field at the top level.
 */
bool Proto_wrapper::extract(int index)
{
  const Reflection *refl= message->GetReflection();
  DBUG_ASSERT(refl);

  std::vector<const FieldDescriptor *> fdescs;
  refl->ListFields(*message, &fdescs);

  const FieldDescriptor *fdesc= fdescs[0];
  if (!fdesc->is_repeated())
    return true;

  if (fdescs.size() != 1)
  {
    setNull();
    return false;
  }

  int size= refl->FieldSize(*message, fdesc);
  if (index < 0 || index >= size)
  {
    setNull();
    return false;
  }

  setRepeatedIndex(index);
  setLeaf();

  return false;
}

bool update_message(Message *message, String *inner_msg, Item *value)
{
  const Descriptor *desc = message->GetDescriptor();
  std::string field_name(inner_msg->c_ptr());
  const FieldDescriptor *fdesc = desc->FindFieldByName(field_name);

  if (!fdesc)
    return true;

  const Reflection *refl = message->GetReflection();
  DBUG_ASSERT(refl);

  String *val, buf;
  val = value->val_str(&buf);

  if (value->type() == Item::NULL_ITEM || !val)
  {
    Message *m = refl->ReleaseMessage(message, fdesc);
    if (m)
      delete m;
  }
  else
  {
    std::stringstream ss(std::string(val->c_ptr(),
                                     val->c_ptr() + val->length()));
    io::IstreamInputStream iis(&ss, -1);
    Message *mutable_msg = refl->MutableMessage(message, fdesc);
    DBUG_ASSERT(mutable_msg);

    if (TextFormat::Parse(&iis, mutable_msg) == false)
    {
      DBUG_PRINT("error", ("Text format parse error."));
      return true;
    }
  }

  return false;
}

bool update_field(Message *message, const FieldDescriptor *fdesc, Item *value)
{
  const Reflection *refl = message->GetReflection();

  if (value->type() == Item::NULL_ITEM)
  {
    refl->ClearField(message, fdesc);
    return false;
  }

  if (fdesc->is_repeated())
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "updating PROTOBUF repeated fields.");
    return true;
  }

  switch (fdesc->type())
  {
    case FieldDescriptor::TYPE_STRING:
    {
      String *val, buf;
      val = value->val_str(&buf);
      if (val)
      {
        std::string val_str(val->c_ptr());
        refl->SetString(message, fdesc, val_str);
      }
      break;
    }
    case FieldDescriptor::TYPE_DOUBLE:
      refl->SetDouble(message, fdesc, value->val_real());
      break;
    case FieldDescriptor::TYPE_FLOAT:
      refl->SetFloat(message, fdesc, (float)value->val_real());
      break;
    case FieldDescriptor::TYPE_INT32:
      refl->SetInt32(message, fdesc, (int32_t)value->val_int());
      break;
    case FieldDescriptor::TYPE_INT64:
      refl->SetInt64(message, fdesc, (int64_t)value->val_int());
      break;
    case FieldDescriptor::TYPE_UINT32:
      refl->SetUInt32(message, fdesc, (uint32_t)value->val_uint());
      break;
    case FieldDescriptor::TYPE_UINT64:
      refl->SetUInt64(message, fdesc, (uint64_t)value->val_uint());
      break;
    case FieldDescriptor::TYPE_BOOL:
      refl->SetBool(message, fdesc, value->val_bool());
      break;
    default:
    {
      DBUG_PRINT("error", ("Couldn't update field: %s\n",
                           fdesc->name().c_str()));
      return true;
    }
  }
  return false;
}

Message *update_top_level(Message *message, String *field, Item *value)
{
  // Check if the field is defined in the proto definition.
  const Descriptor *desc = message->GetDescriptor();
  std::string field_name(field->c_ptr());
  const FieldDescriptor *fdesc = desc->FindFieldByName(field_name);

  if (!fdesc)
    return NULL;

  const Reflection *refl = message->GetReflection();
  DBUG_ASSERT(refl);

  if (fdesc->type() == FieldDescriptor::TYPE_MESSAGE)
    return refl->MutableMessage(message, fdesc);

  if (update_field(message, fdesc, value))
    return NULL;

  return message;
}

bool Proto_wrapper::update(List<String> *field_path, Item *value)
{
  List_iterator_fast<String> it_path(*field_path);
  Message *current_msg = message;
  Message *parent = NULL;
  String *field, *save_fld_name;

  /**
   * TODO(fanton): Remove the use of save_fld_name/parent variables.
   * Currently we need this in a very specific case, that when the
   * field_path ends up on an inner message, not a primitive type. We
   * need to keep that message name, in order to have access to its
   * FieldDescriptor inside the 'parent' message (which is also only
   * needed only in this case.
   */
  while (true) {
    save_fld_name = field;
    field = it_path++;
    if (!field)
      break;

    Message *msg = update_top_level(current_msg, field, value);
    if (msg == current_msg)
      return false;
    else if (msg == NULL)
      return true;
    else
    {
      parent = current_msg; // Keep a pointer to the container message.
      current_msg = msg;
    }
  }

  if (parent)
  {
    /**
     * If we are here, it means that the path was valid, but we didn't end
     * up on a primitive Protobuf type, so there's must be an inner message
     * to update.
     */
    if (update_message(parent, save_fld_name, value))
    {
      String *val, buf;
      val = value->val_str(&buf);

      if (val)
        my_error(ER_INVALID_PROTO_TEXT, MYF(0),
                 "not a PROTOBUF text, may need CAST", 0, val->c_ptr());
      else
        my_error(ER_INVALID_PROTO_TEXT, MYF(0),
                 "not a PROTOBUF text, may need CAST", 0, "NULL");
      return true;
    }
    return false;
  }

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
    if (!is_leaf)
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
    // We normally should never reach this branch, but if we do, than
    // "try" to announce that the value is NULL somehow.
    val_ptr->append("NULL");
  }

  return ret;
}

bool Proto_wrapper::absorb_message(const Reflection *refl,
                                   const FieldDescriptor *fdesc)
{
  const Message& msg = refl->GetMessage(*message, fdesc);
  message = (Message *)&msg;
  return false;
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
      if (fdescs[0]->is_repeated())
        return refl->GetRepeatedDouble(*message, fdescs[0], repeated_index);
      return refl->GetDouble(*message, fdescs[0]);
    case FieldDescriptor::TYPE_FLOAT:
      if (fdescs[0]->is_repeated())
        return (double)refl->GetRepeatedFloat(*message, fdescs[0], repeated_index);
      return (double)refl->GetFloat(*message, fdescs[0]);
    case FieldDescriptor::TYPE_INT32:
      if (fdescs[0]->is_repeated())
        return (double)refl->GetRepeatedInt32(*message, fdescs[0], repeated_index);
      return (double)refl->GetInt32(*message, fdescs[0]);
    case FieldDescriptor::TYPE_INT64:
      if (fdescs[0]->is_repeated())
        return (double)refl->GetRepeatedInt64(*message, fdescs[0], repeated_index);
      return (double)refl->GetInt64(*message, fdescs[0]);
    case FieldDescriptor::TYPE_UINT32:
      if (fdescs[0]->is_repeated())
        return (double)refl->GetRepeatedUInt32(*message, fdescs[0], repeated_index);
      return (double)refl->GetUInt32(*message, fdescs[0]);
    case FieldDescriptor::TYPE_UINT64:
      if (fdescs[0]->is_repeated())
        return (double)refl->GetRepeatedUInt64(*message, fdescs[0], repeated_index);
      return (double)refl->GetUInt64(*message, fdescs[0]);
    case FieldDescriptor::TYPE_STRING:
    {
      char *end_not_used;
      int not_used;
      string str;

      if (fdescs[0]->is_repeated())
        str= refl->GetRepeatedString(*message, fdescs[0], repeated_index);
      else
        str= refl->GetString(*message, fdescs[0]);
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
  const Reflection *refl= message->GetReflection();
  DBUG_ASSERT(refl);
  std::vector<const FieldDescriptor *> fdescs;

  refl->ListFields(*message, &fdescs);
  val_ptr->length(0);
  for (uint32 i= 0; i< fdescs.size(); ++i)
  {
    const FieldDescriptor *fdesc= fdescs[i];
    std::string out_str;
    if (!fdesc->is_repeated())
      TextFormat::PrintFieldValueToString(*message, fdesc, -1, &out_str);
    else
    {
      int size= refl->FieldSize(*message, fdesc);

      // If we didn't extract a field, print all values.
      if (repeated_index == -1)
      {
        out_str= "[";

        for (int i= 0; i < size; i++)
        {
          std::string tmp;
          TextFormat::PrintFieldValueToString(*message, fdesc, i, &tmp);
          out_str.append(tmp);
          out_str.append(", ");
        }
        if (size > 0)
          out_str.erase(out_str.size() - 2);
        out_str.append("]");
      }
      else
      {
        TextFormat::PrintFieldValueToString(*message, fdesc,
                                            repeated_index, &out_str);
      }
    }

    // If we have a string, delete the surrounding quotes to keep
    // consistency with MySQL strings.
    if (fdesc->type() == FieldDescriptor::TYPE_STRING)
    {
      out_str.erase(0, 1);
      out_str.erase(out_str.size() - 1);
    }
    val_ptr->append(out_str.c_str(), out_str.length());
    val_ptr->append(" ");
  }

  // If we did at least one iteration, delete the trailing whitespace.
  if (val_ptr->length() > 0)
    val_ptr->chop();
  return true;
}

// Compare two protobuf wrappers. This function considers two
// Proto_wrappers to be equal iff their messages binary form is exactly
// the same. There's one exception though, if one of the wrappers comes
// from an extract (isLeaf() returns true) it means that probably the
// user just wants to compare the values so we only compare the values.
int Proto_wrapper::compare(Proto_wrapper *other)
{
  std::stringstream oss1, oss2;

  if (isLeaf() || other->isLeaf())
  {
    String s1, s2;
    to_text_only_vals(&s1);
    other->to_text_only_vals(&s2);

    oss1.write(s1.c_ptr(), s1.length());
    oss2.write(s2.c_ptr(), s2.length());
  }
  else
  {
    message->SerializeToOstream(&oss1);
    other->get_message()->SerializeToOstream(&oss2);
  }

  return oss1.str().compare(oss2.str());
}

// Compare a protobuf wrapper to a string. This function takes the
// string, converts it to a Proto_wrapper using the descriptor of the
// current Proto_wrapper and than calls the compare function for two
// Proto_wrappers.
int Proto_wrapper::compare(String *other)
{
  Proto_wrapper other_wr;
  Proto_manager& proto_mgr = Proto_manager::get_singleton();
  const Descriptor* desc = message->GetDescriptor();

  if (!isLeaf())
  {
    if (proto_mgr.construct_wrapper_message(desc, other, &other_wr))
      return -1;
  }
  else
  {
    String *val, buf;
    val = getLeafName(&buf);
    if (proto_mgr.construct_wrapper_value(desc, other, &other_wr, val))
      return -1;
  }

  return compare(&other_wr);
}

// Return the name of this Proto_wrapper's single field. If it's not a
// leaf (not a single field) then return the name of the first field.
String *Proto_wrapper::getLeafName(String *buf)
{
  const Reflection *refl = message->GetReflection();
  DBUG_ASSERT(refl);
  std::vector<const FieldDescriptor *> fdescs;

  refl->ListFields(*message, &fdescs);

  if (fdescs.size() == 0)
    return NULL;

  buf->length(0);
  buf->append(fdescs[0]->name().c_str(), fdescs[0]->name().length());

  return buf;
}

// Write the proto definition to "output".
bool Proto_manager::get_definition(String *file_path, String *field,
                                   MEM_ROOT *mem_root, String *output)
{
  file_path->append(PROTO_FILE_EXTENSION);

  LEX_STRING file_name = file_path->lex_string();
  struct proto_file_data data;

  if (!read_proto_file(&data, &file_name, mem_root))
  {
    DBUG_ASSERT(0);
    return true;
  }
  if (!find_proto_definition(&data, field, output)) {
    DBUG_ASSERT(0);
    return true;
  }
  return false;
}

// Clear the map entries with keys containing 'text'.
void Proto_manager::clear_protobuf_map(const char *text)
{
  std::map<std::string, Descriptor* >::iterator it;

  for (it = descriptor_map.begin(); it != descriptor_map.end();)
  {
    if (it->first.find(text) != std::string::npos)
      descriptor_map.erase(it++);
    else
      it++;
  }
}

// Clear the map entries with keys associated to db 'db' and table
// 'table'.
void Proto_manager::clear_protobuf_map(const char *db, const char *table)
{
  String field_path;
  std::map<std::string, Descriptor* >::iterator it;

  field_path.append(db);
  field_path.append("/");
  field_path.append(table);

  for (it = descriptor_map.begin(); it != descriptor_map.end();)
  {
    if (it->first.find(field_path.c_ptr()) == 0)
      descriptor_map.erase(it++);
    else
      it++;
  }
}
