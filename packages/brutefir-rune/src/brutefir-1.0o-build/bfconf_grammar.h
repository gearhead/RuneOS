/*
 * (c) Copyright 2001, 2002 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
#ifndef _BFCONF_GRAMMAR_H_
#define _BFCONF_GRAMMAR_H_

#include <stdio.h>

#include "defs.h"
#include "bfmod.h"

#define EOS     BF_LEX_EOS /* end of statement (;) */
#define LBRACE  BF_LEX_LBRACE /* { */
#define RBRACE  BF_LEX_RBRACE /* } */
#define COMMA   BF_LEX_COMMA /* , */
#define SLASH   BF_LEX_SLASH /* / */

#define REAL    BF_LEXVAL_REAL
#define BOOLEAN BF_LEXVAL_BOOLEAN
#define STRING  BF_LEXVAL_STRING
#define FIELD   BF_LEXVAL_FIELD

#define COEFF   200
#define INPUT   201
#define OUTPUT  202
#define FILTER  203

extern union bflexval yylval;
extern FILE *yyin;
extern int lexlineno;

int
yylex(void);

void
parse_error(const char msg[]);

#endif
