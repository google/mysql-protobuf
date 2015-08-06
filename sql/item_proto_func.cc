/* PROTOBUF Function items used by mysql */

#include "item_proto_func.h"
#include "sql_class.h"

bool Item_func_protobuf_extract::val_proto(String *result)
{

  if (args[0]->null_value) {
          null_value= true;
          return true;
  }

  String out;

  /**
   * TODO(fanton): The val_str call below will eventually result to a
   * Field_blob::val_str(&out) call. In the future we'll have to insert a
   * ProtoWrapper class in between in order to have more control over
   * the actual protobuf value, being it binary or text.
   */
  args[0]->val_str(&out);
  result->length(0);
  result->append("I will extract from: ");
  result->append(out);
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
    ok= arg->val_proto(result);
  }

  return ok;
}
