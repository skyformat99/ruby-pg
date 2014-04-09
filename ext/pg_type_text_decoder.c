/*
 * pg_column_map.c - PG::ColumnMap class extension
 * $Id$
 *
 */

#include "pg.h"
#include "util.h"
#include <inttypes.h>

VALUE rb_mPG_Type_TextDecoder;
VALUE rb_cPG_Type_TextDecoder_Simple;
VALUE rb_cPG_Type_TextDecoder_Composite;
static ID s_id_call;


static VALUE
pg_type_dec_text_boolean(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	if (len < 1) {
		rb_raise( rb_eTypeError, "wrong data for text boolean converter in tuple %d field %d", tuple, field);
	}
	return *val == 't' ? Qtrue : Qfalse;
}

VALUE
pg_type_dec_text_string(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	VALUE ret = rb_tainted_str_new( val, len );
#ifdef M17N_SUPPORTED
	ENCODING_SET_INLINED( ret, enc_idx );
#endif
	return ret;
}

static VALUE
pg_type_dec_text_integer(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	return rb_cstr2inum(val, 10);
}

static VALUE
pg_type_dec_text_float(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	return rb_float_new(strtod(val, NULL));
}

static VALUE
pg_type_dec_text_bytea(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	unsigned char *to;
	size_t to_len;
	VALUE ret;

	to = PQunescapeBytea( (unsigned char *)val, &to_len);

	ret = rb_tainted_str_new((char*)to, to_len);
	PQfreemem(to);

	return ret;
}

/*
 * Array parser functions are thankfully borrowed from here:
 * https://github.com/dockyard/pg_array_parser
 */
static VALUE
read_array(t_pg_type *conv, int *index, char *c_pg_array_string, int array_string_length, char *word, int enc_idx, int tuple, int field, t_pg_type_dec_func dec_func)
{
	/* Return value: array */
	VALUE array;
	int word_index = 0;

	/* The current character in the input string. */
	char c;

	/*  0: Currently outside a quoted string, current word never quoted
	*  1: Currently inside a quoted string
	* -1: Currently outside a quoted string, current word previously quoted */
	int openQuote = 0;

	/* Inside quoted input means the next character should be treated literally,
	* instead of being treated as a metacharacter.
	* Outside of quoted input, means that the word shouldn't be pushed to the array,
	* used when the last entry was a subarray (which adds to the array itself). */
	int escapeNext = 0;

	array = rb_ary_new();

	/* Special case the empty array, so it doesn't need to be handled manually inside
	* the loop. */
	if(((*index) < array_string_length) && c_pg_array_string[(*index)] == '}')
	{
		return array;
	}

	for(;(*index) < array_string_length; ++(*index))
	{
		c = c_pg_array_string[*index];
		if(openQuote < 1)
		{
			if(c == ',' || c == '}')
			{
				if(!escapeNext)
				{
					if(openQuote == 0 && word_index == 4 && !strncmp(word, "NULL", word_index))
					{
						rb_ary_push(array, Qnil);
					}
					else
					{
						VALUE val;
						word[word_index] = 0;
						val = dec_func(conv, word, word_index, tuple, field, enc_idx);
						rb_ary_push(array, val);
					}
				}
				if(c == '}')
				{
					return array;
				}
				escapeNext = 0;
				openQuote = 0;
				word_index = 0;
			}
			else if(c == '"')
			{
				openQuote = 1;
			}
			else if(c == '{')
			{
				(*index)++;
				rb_ary_push(array, read_array(conv, index, c_pg_array_string, array_string_length, word, enc_idx, tuple, field, dec_func));
				escapeNext = 1;
			}
			else
			{
				word[word_index] = c;
				word_index++;
			}
		}
		else if (escapeNext) {
			word[word_index] = c;
			word_index++;
			escapeNext = 0;
		}
		else if (c == '\\')
		{
			escapeNext = 1;
		}
		else if (c == '"')
		{
			openQuote = -1;
		}
		else
		{
			word[word_index] = c;
			word_index++;
		}
	}

	return array;
}

static VALUE
pg_type_dec_text_array_helper(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx, t_pg_type_dec_func dec_func)
{
	/* create a buffer of the same length, as that will be the worst case */
	char *word = xmalloc(len + 1);
	int index = 1;

	VALUE return_value = read_array(conv, &index, val, len, word, enc_idx, tuple, field, dec_func);
	free(word);
	return return_value;
}

static VALUE
pg_type_dec_text_in_ruby(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	VALUE string = pg_type_dec_text_string(conv, val, len, tuple, field, enc_idx);
	return rb_funcall( conv->dec_obj, s_id_call, 3, string, INT2NUM(tuple), INT2NUM(field) );
}

static VALUE
pg_type_dec_text_array(t_pg_type *conv, char *val, int len, int tuple, int field, int enc_idx)
{
	t_pg_composite_type *comp_conv = (t_pg_composite_type *)conv;
	if( comp_conv->elem ){
		if( comp_conv->elem->dec_func ){
			return pg_type_dec_text_array_helper(comp_conv->elem, val, len, tuple, field, enc_idx, comp_conv->elem->dec_func);
		}else{
			return pg_type_dec_text_array_helper(comp_conv->elem, val, len, tuple, field, enc_idx, pg_type_dec_text_in_ruby);
		}
	}else{
		/* no element decoder defined -> use std String conversion */
		return pg_type_dec_text_array_helper(comp_conv->elem, val, len, tuple, field, enc_idx, pg_type_dec_text_string);
	}
}


static void
define_decoder( const char *name, t_pg_type_dec_func dec_func, VALUE klass, VALUE nsp )
{
  VALUE type_obj = Data_Wrap_Struct( klass, NULL, NULL, dec_func );
  rb_iv_set( type_obj, "@name", rb_obj_freeze(rb_str_new_cstr(name)) );
  rb_define_const( nsp, name, type_obj );

  RB_GC_GUARD(type_obj);
}

void
init_pg_type_text_decoder()
{
	s_id_call = rb_intern("call");

	rb_mPG_Type_TextDecoder = rb_define_module_under( rb_mPG_Type, "TextDecoder" );

	rb_cPG_Type_TextDecoder_Simple = rb_define_class_under( rb_mPG_Type_TextDecoder, "Simple", rb_cObject );
	rb_define_attr( rb_cPG_Type_TextDecoder_Simple, "name", 1, 0 );
	define_decoder( "Boolean", pg_type_dec_text_boolean, rb_cPG_Type_TextDecoder_Simple, rb_mPG_Type_TextDecoder );
	define_decoder( "Integer", pg_type_dec_text_integer, rb_cPG_Type_TextDecoder_Simple, rb_mPG_Type_TextDecoder );
	define_decoder( "Float", pg_type_dec_text_float, rb_cPG_Type_TextDecoder_Simple, rb_mPG_Type_TextDecoder );
	define_decoder( "Text", pg_type_dec_text_string, rb_cPG_Type_TextDecoder_Simple, rb_mPG_Type_TextDecoder );
	define_decoder( "Bytea", pg_type_dec_text_bytea, rb_cPG_Type_TextDecoder_Simple, rb_mPG_Type_TextDecoder );

	rb_cPG_Type_TextDecoder_Composite = rb_define_class_under( rb_mPG_Type_TextDecoder, "Composite", rb_cObject );
	rb_define_attr( rb_cPG_Type_TextDecoder_Composite, "name", 1, 0 );
	define_decoder( "Array", pg_type_dec_text_array, rb_cPG_Type_TextDecoder_Composite, rb_mPG_Type_TextDecoder );
}
