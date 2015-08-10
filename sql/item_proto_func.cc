/* PROTOBUF Function items used by mysql */

#include "item_proto_func.h"
#include "sql_class.h"
#include "proto_manager.h"      // Proto_wrapper

bool Item_func_protobuf_extract::val_proto(Proto_wrapper *wr)
{

  if (args[0]->null_value || args[1]->null_value)
  {
    null_value= true;
    return true;
  }

  String *field, val;
  field= args[1]->val_str(&val);

  if (args[0]->field_type() != MYSQL_TYPE_PROTOBUF)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), args[0]->full_name());
    return false;
  }

  if (args[0]->val_proto(wr) == false)
  {
    DBUG_PRINT("error", ("Error with val_proto."));
    return false;
  }

  if (wr->extract(field) == false)
  {
    my_error(ER_INVALID_PROTO_FIELD, MYF(0), field->c_ptr(),
             args[0]->full_name());
    return false;
  }
  return true;
}

double Item_proto_func::val_real()
{
        DBUG_ENTER("Item_proto_func::val_real");
        DBUG_RETURN(0.0);
}

longlong Item_proto_func::val_int()
{
        DBUG_ENTER("Item_proto_func::val_int");
        DBUG_RETURN(0);
}

String *Item_proto_func::val_str(String *str)
{
        DBUG_ENTER("Item_proto_func::val_str");
        DBUG_RETURN(str);
}

bool Item_proto_func::get_date(MYSQL_TIME *ltime, my_time_flags_t fuzzydate)
{
        DBUG_ENTER("Item_proto_func::get_date");
        DBUG_RETURN(true);
}

bool Item_proto_func::get_time(MYSQL_TIME *ltime)
{
        DBUG_ENTER("Item_proto_func::get_time");
        DBUG_RETURN(true);
}

my_decimal *Item_proto_func::val_decimal(my_decimal *decimal_value)
{
        DBUG_ENTER("Item_proto_func::val_decimal");
        DBUG_RETURN(decimal_value);
}

/**
  Get a PROTOBUF value from the item indicated by arg_idx.
  @param[in,out] args    the arguments to the function
  @param[in]     arg_idx the argument index

  @return true if a PROTOBUF value (or NULL) is found, else false
*/
bool proto_value(Item **args, uint arg_idx, String *result)
{
  Item *arg= args[arg_idx];
  bool ok= true;

  if (arg->field_type() == MYSQL_TYPE_NULL)
  {
    DBUG_ASSERT(arg->null_value);
  }
  else if (arg->field_type() != MYSQL_TYPE_PROTOBUF)
  {
    // This is not a PROTOBUF value. Give up.
    ok= false;
  }
  else
  {
    Proto_wrapper wr;
    ok= arg->val_proto(&wr);
    if (!ok)
      return false;
    ok= wr.to_text(result);
  }
  return ok;
}
