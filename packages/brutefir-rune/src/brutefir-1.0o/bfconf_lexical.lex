/*
 * (c) Copyright 2001 -- Anders Torger
 *
 * This program is open source. For license terms, see the LICENSE file.
 *
 */
%{
#include <stdio.h>
#include <stdlib.h>
  
#include "defs.h"
#include "bfconf_grammar.h"

int lexlineno = 0;
    
%}
%option nounput
%option noyywrap
%option noyylineno
%option noreject
%option fast
%option 8bit

DIGIT [0-9]
ANUM [A-Za-z_0-9]

%%

"\n" {
    lexlineno++;
}

"{"       { return LBRACE; }
"}"       { return RBRACE; }
","       { return COMMA; }
"/"       { return SLASH; }
";"       { return EOS; }

"#"[^\n]*  /* remove one-line comments */

[ \t\r]+ /* remove whitespace */

"coeff"  { return COEFF; }
"input"  { return INPUT; }
"output" { return OUTPUT; }
"filter" { return FILTER; }
"route"  { return FILTER; } /* backwards compability */

"true" {
    yylval.boolean = true;
    return BOOLEAN;
}

"false" {
    yylval.boolean = false;
    return BOOLEAN;
}

"\""("\\\""|[^\"])*"\"" {
    int n, slen;
    slen = strlen(yytext) - 1;
    yytext[slen] = '\0';
    for (n = 0; n < slen - 1; n++) {
        switch ((int)yytext[n]) {
        case '\\':
            memmove(&yytext[n], &yytext[n+1], slen - n);
            slen--;
            switch ((int)yytext[n]) {
            case 'n':
                yytext[n] = '\n';
                break;
            case 't':
                yytext[n] = '\t';
                break;
            default:
                break;
            }
            break;
        case '\n':
            lexlineno++;
            break;
        }
    }
    yylval.string = &yytext[1];
    return STRING;   
}

{ANUM}+":" {
    yylval.field = yytext;
    yylval.field[strlen(yylval.field)-1] = '\0';
    return FIELD;
}

("+"|"-")?{DIGIT}*"."?{DIGIT}+("e"("+"|"-"){DIGIT}{2})? {
    yylval.real = atof(yytext);
    return REAL;
}

<<EOF>> {
    return EOF;
}

. {
    parse_error("unrecognised token.\n");
}

%%
