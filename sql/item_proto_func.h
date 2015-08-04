#ifndef ITEM_PROTO_FUNC_INCLUDED
#define ITEM_PROTO_FUNC_INCLUDED

#include "sql_string.h"
#include "item_strfunc.h"       // Item_func
#include "proto_manager.h"      // Proto_wrapper

/* PROTOBUF function support  */

class Proto_wrapper;

/**
  Base class for all item functions that return a PROTOBUF value.
*/
class Item_proto_func : public Item_func
{
public:
  Item_proto_func(const POS &pos, Item *a) : Item_func(pos, a)
  {}
  Item_proto_func(const POS &pos, Item *a, Item *b) : Item_func(pos, a, b)
  {}
  Item_proto_func(const POS &pos, PT_item_list *a) : Item_func(pos, a)
  {}

  enum_field_types field_type() const { return MYSQL_TYPE_PROTOBUF; }

  void fix_length_and_dec()
  {
    max_length= MAX_BLOB_WIDTH;
    maybe_null= true;
    collation.collation= &my_charset_utf8mb4_bin;
  }
  enum Item_result result_type () const { return STRING_RESULT; }
  String *val_str(String *arg);
  bool get_date(MYSQL_TIME *ltime, my_time_flags_t fuzzydate);
  bool get_time(MYSQL_TIME *ltime);
  longlong val_int();
  double val_real();
  my_decimal *val_decimal(my_decimal *decimal_value);

   void update_null_value ()
   {
       Proto_wrapper wr;
       val_proto(&wr);
       if (wr.isNull())
         null_value= true;
       else
         null_value= false;
   }
  Item_result cast_to_int_type () const { return INT_RESULT; }
};

/**
  Represents the PROTOBUF function PROTO_EXTRACT().
*/
class Item_func_protobuf_extract :public Item_proto_func
{
public:
  Item_func_protobuf_extract(const POS &pos, PT_item_list *a)
    : Item_proto_func(pos, a)
  {}

  const char *func_name() const
  {
    return "proto_extract";
  }

  bool val_proto(Proto_wrapper *wr);
};

/**
  Represents the PROTOBUF function PROTOBUF_UPDATE()
*/
class Item_func_protobuf_update :public Item_proto_func
{
  String m_doc_value;
  String m_path_value;
public:
  Item_func_protobuf_update(const POS &pos, PT_item_list *a)
    : Item_proto_func(pos, a)
  {}

  const char *func_name() const
  {
    return "protobuf_update";
  }

  bool val_proto(Proto_wrapper *wr);
};

bool proto_value(Item **args, uint arg_idx, Proto_wrapper *wr);
#endif /* ITEM_PROTO_FUNC_INCLUDED */
