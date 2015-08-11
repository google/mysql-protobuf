/* PROTOBUF Function items used by mysql */

#include "item_proto_func.h"
#include "sql_class.h"
#include "proto_manager.h"      // Proto_wrapper

bool Item_func_protobuf_extract::val_proto(Proto_wrapper *wr)
{

  if (args[0]->null_value || args[1]->null_value)
  {
    null_value= true;
    wr->setNull();
    return true;
  }

  if (args[0]->field_type() != MYSQL_TYPE_PROTOBUF)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), args[0]->full_name());
    return false;
  }

  if (args[0]->val_proto(wr) == false)
  {
    DBUG_PRINT("error", ("Error converting to val_proto: %s",
                args[0]->full_name()));
    return false;
  }

  if (wr->isNull())
    return true;

  if (arg_count == 2)
  {
    String *proto_path, val, dot;
    int offset= 0, dot_start;

    dot.append(".");
    proto_path= args[1]->val_str(&val);
    proto_path->append(".");
    dot_start= proto_path->strstr(dot, offset);

    while (dot_start > 0 && (uint32)dot_start < proto_path->length())
    {
      String proto_field= proto_path->substr(offset, dot_start - offset);

      if (wr->extract(&proto_field))
      {
        my_error(ER_INVALID_PROTO_FIELD, MYF(0), proto_field.c_ptr(),
                 args[0]->full_name());
        proto_path->chop();
        return false;
      }

      if (wr->isNull())
      {
        proto_path->chop();
        return true;
      }

      offset= dot_start + 1;
      dot_start= proto_path->strstr(dot, offset);
    }
    proto_path->chop();
  }
  else
  {
    for (uint32 i= 1; i< arg_count; i++)
    {
      String *proto_field, val;

      // Test against repeated fields selector.
      if (i == arg_count - 1 && args[i]->type() == Item::INT_ITEM)
      {
        int index= args[i]->val_int();
        if (index < 0)
        {
          my_error(ER_NEGATIVE_PROTO_REPEATED_INDEX, MYF(0), proto_field->c_ptr(),
                   args[0]->full_name());
          return false;
        }
        if (wr->extract(index))
        {
          my_error(ER_INVALID_PROTO_REPEATED_FIELD, MYF(0), proto_field->c_ptr(),
                   args[0]->full_name());
          return false;
        }
      }
      else
      {
        proto_field= args[i]->val_str(&val);

        if (!proto_field)
        {
          my_error(ER_INVALID_PROTO_FIELD, MYF(0), "NULL",
                   args[0]->full_name());
          return false;
        }
        if (wr->extract(proto_field))
        {
          my_error(ER_INVALID_PROTO_FIELD, MYF(0), proto_field->c_ptr(),
                   args[0]->full_name());
          return false;
        }
      }

      if (wr->isNull())
        return true;
    }
  }
  return true;
}

bool Item_func_protobuf_update::val_proto(Proto_wrapper *wr)
{

  if (args[0]->null_value || args[1]->null_value)
  {
    null_value= true;
    wr->setNull();
    return true;
  }

  if (args[0]->field_type() != MYSQL_TYPE_PROTOBUF)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), args[0]->full_name());
    return false;
  }

  if (args[0]->val_proto(wr) == false)
  {
    DBUG_PRINT("error", ("Error converting to val_proto: %s",
                args[0]->full_name()));
    return false;
  }

  wr->unsetNull();

  List<String> field_path;
  for (uint32 i = 1; i< arg_count - 1; i++)
  {
    String *val, buf2;

    val = args[i]->val_str(&buf2);
    field_path.push_back(val);
  }

  if (wr->update(&field_path, args[arg_count - 1]))
  {
    String fpath;
    List_iterator_fast<String> it_path(field_path);
    while (true)
    {
      String *field= it_path++;
      if (!field)
        break;
      fpath.append(field->c_ptr(), field->length());
      fpath.append(".");
    }
    fpath.chop();

    my_error(ER_INVALID_PROTO_FIELD, MYF(0), fpath.c_ptr(),
             args[0]->full_name());
    return false;
  }

  return true;
}

double Item_proto_func::val_real()
{
  Proto_wrapper wr;

  DBUG_ENTER("Item_proto_func::val_real");
  if (field_type() != MYSQL_TYPE_PROTOBUF)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), full_name());
    DBUG_RETURN(0.0);
  }

  if (val_proto(&wr) == false)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), full_name());
    DBUG_RETURN(0.0);
  }

  if (wr.isNull())
    DBUG_RETURN(0.0);

  DBUG_RETURN(wr.val_real());
}

longlong Item_proto_func::val_int()
{
  Proto_wrapper wr;

  DBUG_ENTER("Item_proto_func::val_int");
  if (field_type() != MYSQL_TYPE_PROTOBUF)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), full_name());
    DBUG_RETURN(0);
  }

  if (val_proto(&wr) == false)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), full_name());
    DBUG_RETURN(0);
  }

  if (wr.isNull())
    DBUG_RETURN(0);

  DBUG_RETURN(wr.val_int());
}

String *Item_proto_func::val_str(String *str)
{
  DBUG_ENTER("Item_proto_func::val_str");
  Proto_wrapper wr;

  if (field_type() != MYSQL_TYPE_PROTOBUF)
  {
    my_error(ER_INVALID_PROTO_COLUMN, MYF(0), full_name());
    return str;
  }

  if (val_proto(&wr) == false || wr.isNull())
  {
    null_value= true;
    DBUG_RETURN(NULL);
  }
  wr.to_text(str);
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
  @param[out]    wr      the Proto_wrapper for this value

  @return true if a PROTOBUF value (or NULL) is found, else false
*/
bool proto_value(Item **args, uint arg_idx, Proto_wrapper *wr)
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
    ok= arg->val_proto(wr);
  }
  return ok;
}
