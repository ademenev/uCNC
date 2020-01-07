/*
	Name: parser.c
	Description: Parses Grbl system commands and RS274NGC (GCode) commands
        The RS274NGC parser tries to follow the standard document version 3 as close as possible.
        The parsing is done in 3 steps:
            - Tockenization; Converts the command string to a structure with GCode parameters
            - Validation; Validates the command by checking all the parameters (part 3.5 - 3.7 of the document)
            - Execution; Executes the command by the orther set in part 3.8 of the document.
			
	Copyright: Copyright (c) João Martins 
	Author: João Martins
	Date: 07/12/2019

	uCNC is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version. Please see <http://www.gnu.org/licenses/>

	uCNC is distributed WITHOUT ANY WARRANTY;
	Also without the implied warranty of	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
	See the	GNU General Public License for more details.
*/

#include "config.h"
#include "mcudefs.h"
#include "mcu.h"
#include "grbl_interface.h"
#include "utils.h"
#include "settings.h"
#include "serial.h"
#include "protocol.h"
#include "planner.h"
#include "motion_control.h"
#include "dio_control.h"
#include "cnc.h"
#include "parser.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

//group0 offsets
#define GCODE_GROUP_MOTION      1
#define GCODE_GROUP_PLANE       2
#define GCODE_GROUP_DISTANCE    4
#define GCODE_GROUP_FEEDRATE    8
#define GCODE_GROUP_UNITS       16
#define GCODE_GROUP_CUTTERRAD   32
#define GCODE_GROUP_TOOLLENGTH  64
#define GCODE_GROUP_RETURNMODE  128
//group1 offsets
#define GCODE_GROUP_COORDSYS    1
#define GCODE_GROUP_PATH        2
#define GCODE_GROUP_STOPPING    4
#define GCODE_GROUP_TOOLCHANGE  8
#define GCODE_GROUP_SPINDLE     16
#define GCODE_GROUP_COOLANT     32
#define GCODE_GROUP_ENABLEOVER  64
#define GCODE_GROUP_NONMODAL    128
//word0 offsets
#define GCODE_WORD_X    1
#define GCODE_WORD_Y    2
#define GCODE_WORD_Z    4
#define GCODE_WORD_A    8
#define GCODE_WORD_B    16
#define GCODE_WORD_C    32
#define GCODE_WORD_D    64
#define GCODE_WORD_F    128
//word1 offsets
#define GCODE_WORD_I    GCODE_WORD_X //matches X axis bit
#define GCODE_WORD_J    GCODE_WORD_Y //matches Y axis bit
#define GCODE_WORD_K    GCODE_WORD_Z //matches Z axis bit
#define GCODE_WORD_H    8
#define GCODE_WORD_L    16
#define GCODE_WORD_P    32
#define GCODE_WORD_Q    64
#define GCODE_WORD_R    128
//word2 offsets
#define GCODE_WORD_S    1
#define GCODE_WORD_T    2
#define GCODE_WORD_N    4

#define GCODE_MAIN_AXIS (GCODE_WORD_X | GCODE_WORD_Y | GCODE_WORD_Z)
#define GCODE_ALL_AXIS (GCODE_MAIN_AXIS | GCODE_WORD_A | GCODE_WORD_B | GCODE_WORD_C)
#define GCODE_XYPLANE_AXIS (GCODE_WORD_X | GCODE_WORD_Y)
#define GCODE_XZPLANE_AXIS (GCODE_WORD_X | GCODE_WORD_Z)
#define GCODE_YZPLANE_AXIS (GCODE_WORD_Y | GCODE_WORD_Z)

typedef struct
{
    //group1
    uint8_t motion : 4;
    uint8_t plane : 2;
    uint8_t distance_mode : 1;
    uint8_t feedrate_mode : 1;

    uint8_t units : 1;
    uint8_t cutter_radius_compensation : 2;
    uint8_t tool_length_offset : 1;
    uint8_t return_mode : 1;
    uint8_t coord_system : 3;

    uint8_t path_mode : 2;
    uint8_t stopping : 3;
    uint8_t tool_change : 1;
    uint8_t spindle_turning : 2;

    uint8_t coolant : 2;
    uint8_t feed_speed_override : 1;
    uint8_t nonmodal : 4; //reset to 0 in every line (non persistent)
    uint8_t : 1; //unused
} parser_groups_t;

typedef struct
{
    float xyzabc[AXIS_COUNT];
    float d;
    float f;
    float h;
    float ijk[3];
    uint8_t l;
    float p;
    float q;
    float r;
    float s;
    uint8_t t;
} parser_words_t;

typedef struct
{
    uint32_t linenum; 
    parser_groups_t groups;
    parser_words_t words;
} parser_state_t;

typedef struct
{
	/*bool relative_mode;
	float feedrate;*/
    float g28home[AXIS_COUNT];
    float g30home[AXIS_COUNT];
    float g92offset[AXIS_COUNT];
    //uint8_t coord_sys_index;
    float coord_sys[COORD_SYS_COUNT][AXIS_COUNT];
} parser_parameters_t;

static parser_state_t parser_state;
static parser_parameters_t parser_parameters;
static float parser_offset_pos[AXIS_COUNT];
static float parser_max_feed_rate;
//contains all bitflags for the used groups/words on the parsed command
static uint8_t parser_group0;
static uint8_t parser_group1;
static uint8_t parser_word0;
static uint8_t parser_word1;
static uint8_t parser_word2;
static bool parser_checkmode;

/*
	Parses a string to number (real)
	If the number is an integer the isinteger flag is set
	The string pointer is also advanced to the next position
*/
bool parser_get_float(float *value, bool *isinteger)
{
    bool isnegative = false;
    bool isfloat = false;
    uint32_t intval = 0;
    uint8_t fpcount = 0;
    bool result = false;

    char c = serial_peek();

    if (c == '-')
    {
        isnegative = true;
        serial_getc();
    }
    else if (c == '+')
    {
        serial_getc();
    }
    else if (c == '.')
    {
        isfloat = true;
        serial_getc();
    }

    for(;;)
    {
    	c = serial_peek();
        uint8_t digit = (uint8_t)c - 48;
        if (digit <= 9)
        {
            intval = fast_mult10(intval) + digit;
            if (isfloat)
            {
                fpcount++;
            }

            result = true;
        }
        else if (c == '.' && !isfloat)
        {
            isfloat = true;
            //result = false;
        }
        else
        {
        	if(!result)
        	{
        		return result;
			}
            break;
        }

        serial_getc();
    }
    
    *value = (float)intval;
 
    do
    {
        if(fpcount>=3)
        {
            *value *= 0.001f;
            fpcount -= 3;
        }
        
		if(fpcount>=2)
        {
            *value *= 0.01f;
            fpcount -= 2;
        }
        
		if(fpcount>=1)
        {
            *value *= 0.1f;
            fpcount -= 1;
        }

    } while (fpcount !=0 );
    

    *isinteger = !isfloat;
    
    if(isnegative)
    {
    	*value = -*value;
	}
	
    return result;

}

/*
	parses comments as defined in the RS274
	Suports nested comments
	If comment is not closed returns an error
*/
/*
static bool parser_get_comment()
{
	uint8_t comment_nest = 1;
	
	#ifdef ECHO_CMD
	protocol_echo_mute();
    #endif
    
	for(;;)
	{
		char c = serial_getc();
		switch(c)
		{
			case '(':
				comment_nest++;
				break;
			case ')':
				comment_nest--;
				if(comment_nest == 0)
				{
					#ifdef ECHO_CMD
					protocol_echo_unmute();
				    #endif
					return true;//comment sucessfully consumed
				}
				break;
			case '\n':
				#ifdef ECHO_CMD
				protocol_echo_unmute();
			    #endif
                return false;
		}
	}
	
	#ifdef ECHO_CMD
	protocol_echo_unmute();
    #endif
    return false;	
}
*/
/*
	STEP 1
	Fetches the next line from the mcu communication buffer and preprocesses the string
	In the preprocess these steps are executed
		1. Whitespaces are ignored
		2. Comments are parsed (nothing is done besides parsing for now)
		3. All letters are upper-cased
		4. Checks number formats in all words
		5. Checks for modal groups and words collisions
*/
static uint8_t parser_fetch_command(parser_state_t *new_state)
{
    bool hasnumber = false;
    float word_val = 0.0;
    char word = '\0';
    uint8_t code = 0;
    uint8_t wordcount = 0;
    uint8_t mwords = 0;

    //flags for used words and groups
    parser_group0 = 0;
    parser_group1 = 0;
    parser_word0 = 0;
    parser_word1 = 0;
    parser_word2 = 0;

    for(;;)
    {
    	word = serial_getc();
    	bool isinteger = false;

    	switch(word)
		{
			/*case '(':
				if(!parser_get_comment())
                {
                    return STATUS_BAD_COMMENT_FORMAT;//error code
                }
                word = 0;
				break;*/
			case '\n':
				word = '\n'; //EOL marker
				return STATUS_OK;
			default:
                if(word>='a' && word<='z') //uppercase
                {
                    word -= 32;
                }
                
                if(word<'A' || word>'Z')//invalid recognized char
                {
                    return STATUS_EXPECTED_COMMAND_LETTER;
                }
				break;
		}

        //checks if word is followed by number
        hasnumber = parser_get_float(&word_val, &isinteger);
		if(!hasnumber && word)
		{
			return STATUS_BAD_NUMBER_FORMAT;
		}

        switch(word)
        {
            case 'G':
                if(!hasnumber)
                {
                    return STATUS_INVALID_STATEMENT;
                }
                
                code = (uint8_t)floorf(word_val);
                //check mantissa
				uint8_t mantissa = 0;
		        if(!isinteger)
		        {
		        	switch(code)
		        	{
		        		//codes with possible mantissa
		        		case 38:
		        		case 59:
		        		case 92:
		        			mantissa = (uint8_t)floorf((word_val - code)*100.0f);
		        			break;
		        		default:
		        			return STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER;
		        			break;
					}
		        }

                switch(code)
                {
                    //motion codes
                    case 0:
                    case 1:
                    case 2:
                    case 3:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_MOTION))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_MOTION);
                        new_state->groups.motion = code;
                        break;
                    //unsuported
                    case 38://check if 38.2
                        code = 4;
                    	return STATUS_GCODE_UNSUPPORTED_COMMAND;
                        SETFLAG(parser_group0,GCODE_GROUP_MOTION);
                        if(mantissa == 20)
                        {
                            return STATUS_GCODE_UNSUPPORTED_COMMAND;
                        }      
                        else
                        {
                            return STATUS_INVALID_STATEMENT;
                        }
                        break;
                    case 80:
                    case 81:
                    case 82:
                    case 83:
                    case 84:
                    case 85:
                    case 86:
                    case 87:
                    case 88:
                    case 89:
                    	return STATUS_GCODE_UNSUPPORTED_COMMAND;
                    	if(!isinteger)
                    	{
                    		return STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER;
						}
						
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_MOTION))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_MOTION);
                        code -= 75;
                        new_state->groups.motion = code;
                        break;
                    case 17:
                    case 18:
                    case 19:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_PLANE))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_PLANE);
                        code -= 17;
                        new_state->groups.plane = code;
                        break;
                    case 90:
                    case 91:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_DISTANCE))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_DISTANCE);
                        code -= 90;
                        new_state->groups.distance_mode = code;
                        break;
                    case 93:
                    case 94:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_FEEDRATE))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_FEEDRATE);
                        code -= 93;
                        new_state->groups.feedrate_mode = code;
                        break;
                    case 20:
                    case 21:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_UNITS))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_UNITS);  
                        code -= 20;
                        new_state->groups.units = code;
                        break;
                    case 40:
                    case 41:
                    case 42:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_CUTTERRAD))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_CUTTERRAD);
                        code -= 40;
                        new_state->groups.cutter_radius_compensation = code;
                        break;
                    case 43:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_TOOLLENGTH))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_TOOLLENGTH);
                        new_state->groups.tool_length_offset = 0;
                        break;
                    case 49:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_TOOLLENGTH))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_TOOLLENGTH);
                        new_state->groups.tool_length_offset = 1;
                        break;
                    case 98:
                    case 99:
                        if(CHECKFLAG(parser_group0,GCODE_GROUP_RETURNMODE))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group0,GCODE_GROUP_RETURNMODE);
                        code -= 98;
                        new_state->groups.return_mode = code;
                        break;
                    case 54:
                    case 55:
                    case 56:
                    case 57:
                    case 58:
                        if(CHECKFLAG(parser_group1,GCODE_GROUP_COORDSYS))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_COORDSYS);
                        code -= 54;
                        
                        if(code > COORD_SYS_COUNT)
                        {
                        	return STATUS_GCODE_UNSUPPORTED_COORD_SYS;
						}
						new_state->groups.coord_system = code;
                        break;
                    case 59:
                    	if(CHECKFLAG(parser_group1,GCODE_GROUP_COORDSYS))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_COORDSYS);
                        switch(mantissa)
                        {
                        	case 0:
                            	break;
                            case 10:
                            	code += 1;
                            	break;
                            case 20:
                            	code += 2;
                            	break;
                            case 30:
                            	code += 3;
                                break;
                            default:
                                return STATUS_GCODE_UNSUPPORTED_COMMAND;
                        }
                        code -= 54;
                        
                        if(code > COORD_SYS_COUNT)
                        {
                        	return STATUS_GCODE_UNSUPPORTED_COORD_SYS;
						}
						new_state->groups.coord_system = code;
                        break;
                    case 61:
                        if(CHECKFLAG(parser_group1,GCODE_GROUP_PATH))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_PATH);
                        new_state->groups.path_mode = 0;
                        break;
                    case 64:
                        if(CHECKFLAG(parser_group1,GCODE_GROUP_PATH))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_PATH);
                        new_state->groups.path_mode = 1;
                        break;
                    case 4:
                    case 53:
                    case 10:
                    case 28:
                    case 30:
                        //convert code within 4 bits without 
                        //4 = 0
                        //10 = 1
                        //28 = 2
                        //30 = 3
                        //53 = 5
						code = (uint8_t)floor(word_val * 0.10001f);

                        if(CHECKFLAG(parser_group1,GCODE_GROUP_NONMODAL))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_NONMODAL);
                        new_state->groups.nonmodal = code;
                        break;
                    case 92:
                        //92 = 9
                        //92.1 = 10
                        //92.2 = 11
                        //92.3 = 12
                        switch(mantissa)
                        {
                        	case 0:
                        		code = 9;
                        		break;
                        	case 10:
                        		code = 10;
                        		break;
                        	case 20:
                        		code = 11;
                        		break;
                        	case 30:
                        		code = 12;
                        		break;
						}

                        if(CHECKFLAG(parser_group1,GCODE_GROUP_NONMODAL))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_NONMODAL);
                        new_state->groups.nonmodal = code;
                        break;
                    
                    default:
                        return STATUS_GCODE_UNSUPPORTED_COMMAND;
                }
                break;

            case 'M':
                if(!isinteger)
                {
                    return STATUS_GCODE_UNSUPPORTED_COMMAND;
                }
                
                //counts number of M commands
                mwords++;
                code = (uint8_t)(word_val);
                switch(code)
                {
                    case 0:
                    case 1:
                    case 2:
                    case 30:
                    case 60:
                        if(CHECKFLAG(parser_group1,GCODE_GROUP_STOPPING))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_STOPPING);
                        if(code >= 10)
                        {
                            code /= 10;
                        }
                        new_state->groups.stopping = code;
                        break;
                    case 3:
                    case 4:
                    case 5:
                        if(CHECKFLAG(parser_group1,GCODE_GROUP_SPINDLE))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_SPINDLE);
                        code -= 3;
                        new_state->groups.spindle_turning = code;
                        break;
                    case 6:
                        SETFLAG(parser_group1,GCODE_GROUP_TOOLCHANGE);
                        new_state->groups.tool_change = 1;
                        break;
                    case 7:
                        new_state->groups.coolant |= 1;
                        break;
                    case 8:
                        new_state->groups.coolant |= 2;
                        break;
                    case 9:
                        new_state->groups.coolant = 0;
                        break;
                    case 48:
                    case 49:
                        if(CHECKFLAG(parser_group1,GCODE_GROUP_ENABLEOVER))
                        {
                            return STATUS_GCODE_MODAL_GROUP_VIOLATION;
                        }

                        SETFLAG(parser_group1,GCODE_GROUP_ENABLEOVER);
                        code -= 48;
                        new_state->groups.feed_speed_override = code;
                        break;
                    default:
                        return STATUS_GCODE_UNSUPPORTED_COMMAND;
                }
            break;
        case 'N':
            if(CHECKFLAG(parser_word2, GCODE_WORD_N))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word2, GCODE_WORD_N);

            if(!isinteger || wordcount != 0 || word_val < 0)
            {
                return STATUS_GCODE_INVALID_LINE_NUMBER;
            }

            new_state->linenum = trunc(word_val);
            break;
        case 'X':
            if(CHECKFLAG(parser_word0, GCODE_WORD_X))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_X);
            if(hasnumber)
            {
                new_state->words.xyzabc[0] = word_val;
            }
            break;
        case 'Y':
            if(CHECKFLAG(parser_word0, GCODE_WORD_Y))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_Y);
            if(hasnumber)
            {
                new_state->words.xyzabc[1] = word_val;
            }
            break;
        case 'Z':
            if(CHECKFLAG(parser_word0, GCODE_WORD_Z))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_Z);
            if(hasnumber)
            {
                new_state->words.xyzabc[2] = word_val;
            }
            break;
        case 'A':
            if(CHECKFLAG(parser_word0, GCODE_WORD_A))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_A);
            if(hasnumber)
            {
                new_state->words.xyzabc[3] = word_val;
            }
            break;
        case 'B':
            if(CHECKFLAG(parser_word0, GCODE_WORD_B))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_B);
            if(hasnumber)
            {
                new_state->words.xyzabc[4] = word_val;
            }
            break;
        case 'C':
            if(CHECKFLAG(parser_word0, GCODE_WORD_C))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_C);
            if(hasnumber)
            {
                new_state->words.xyzabc[5] = word_val;
            }
            break;
        case 'D':
            if(CHECKFLAG(parser_word0, GCODE_WORD_D))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_D);
            new_state->words.d = word_val;
            break;
        case 'F':
            if(CHECKFLAG(parser_word0, GCODE_WORD_F))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word0, GCODE_WORD_F);
            new_state->words.f = word_val;
            break;
        case 'H':
            if(CHECKFLAG(parser_word1, GCODE_WORD_H))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word1, GCODE_WORD_H);
            new_state->words.h = word_val;
            break;
        case 'I':
            if(CHECKFLAG(parser_word1, GCODE_WORD_I))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word1, GCODE_WORD_I);
            new_state->words.ijk[0] = word_val;
            break;
        case 'J':
            if(CHECKFLAG(parser_word1, GCODE_WORD_J))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word1, GCODE_WORD_J);
            new_state->words.ijk[1] = word_val;
            break;
        case 'K':
            if(CHECKFLAG(parser_word1, GCODE_WORD_K))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word1, GCODE_WORD_K);
            new_state->words.ijk[2] = word_val;
            break;
        case 'L':
            if(CHECKFLAG(parser_word1, GCODE_WORD_L))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            if(!isinteger)
            {
                return STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER;
            }

            SETFLAG(parser_word1, GCODE_WORD_L);
            new_state->words.l= (uint8_t)floorf(word_val);
            break;
        case 'P':
            if(CHECKFLAG(parser_word1, GCODE_WORD_P))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            if(word_val < 0)
            {
            	return STATUS_NEGATIVE_VALUE;
			}

            SETFLAG(parser_word1, GCODE_WORD_P);
            new_state->words.p = word_val;
            break;
        case 'Q':
            if(CHECKFLAG(parser_word1, GCODE_WORD_Q))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word1, GCODE_WORD_Q);
            new_state->words.q = word_val;
            break;
        case 'R':
            if(CHECKFLAG(parser_word1, GCODE_WORD_R))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word1, GCODE_WORD_R);
            new_state->words.r = word_val;
            break;
        case 'S':
            if(CHECKFLAG(parser_word2, GCODE_WORD_S))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            SETFLAG(parser_word2, GCODE_WORD_S);
            new_state->words.s = MIN(word_val, g_settings.spindle_max_rpm);
            break;
        case 'T':
            if(CHECKFLAG(parser_word2, GCODE_WORD_T))
            {
                return STATUS_GCODE_WORD_REPEATED;
            }

            if(!isinteger)
            {
                return STATUS_GCODE_COMMAND_VALUE_NOT_INTEGER;
            }

            SETFLAG(parser_word2, GCODE_WORD_T);
            new_state->words.t = (uint8_t)floorf(word_val);
            break;
        default:
            break;
        }
        wordcount++;
    }
    
    //The string is read and all tockens are identified
    //The command can now be checked for validation before being executed
	return STATUS_OK;
}

/*
	STEP 2
	Validadates command by checking for errors on all G/M Codes
		RS274NGC v3 - 3.5 G Codes
		RS274NGC v3 - 3.6 Input M Codes
		RS274NGC v3 - 3.7 Other Input Codes
*/

static uint8_t parser_validate_command(parser_state_t *new_state)
{
	//pointer to planner position
	float parser_last_pos[AXIS_COUNT];
	planner_get_position(parser_last_pos);
    
    //only alow groups 3, 6 and modal G53
    if(cnc_get_exec_state(EXEC_JOG))
    {
    	if(parser_group0 & ~(GCODE_GROUP_DISTANCE | GCODE_GROUP_UNITS))
    	{
    		return STATUS_INVALID_JOG_COMMAND;
		}
		
		if(parser_group1 & ~GCODE_GROUP_NONMODAL)
		{
			return STATUS_INVALID_JOG_COMMAND;
		}
		
		//if nonmodal other than G53
		if(new_state->groups.nonmodal!=0 && new_state->groups.nonmodal!=5)
		{
			return STATUS_INVALID_JOG_COMMAND;
		}
		
		if((parser_word0 & GCODE_WORD_D) || parser_word1 || (parser_word2 & GCODE_WORD_N))
		{
			return STATUS_INVALID_JOG_COMMAND;
		}
	}
    
    //Check if axis are defined and can be used
    #ifndef AXIS_X
    if(parser_word0 & GCODE_WORD_X)
    {
    	return STATUS_UNDEFINED_AXIS;
	}	
	if(parser_word1 & GCODE_WORD_I)
    {
    	return STATUS_UNDEFINED_AXIS;
	}
	#endif
	#ifndef AXIS_Y
	if(parser_word0 & GCODE_WORD_Y)
    {
    	return STATUS_UNDEFINED_AXIS;
	}	
	if(parser_word1 & GCODE_WORD_J)
    {
    	return STATUS_UNDEFINED_AXIS;
	}
	#endif
	#ifndef AXIS_Z
	if(parser_word0 & GCODE_WORD_Z)
    {
    	return STATUS_UNDEFINED_AXIS;
	}
	if(parser_word1 & GCODE_WORD_K)
    {
    	return STATUS_UNDEFINED_AXIS;
	}	
	#endif
	#ifndef AXIS_A
	if(parser_word0 & GCODE_WORD_A)
    {
    	return STATUS_UNDEFINED_AXIS;
	}	
	#endif
	#ifndef AXIS_B
	if(parser_word0 & GCODE_WORD_B)
    {
    	return STATUS_UNDEFINED_AXIS;
	}	
	#endif
	#ifndef AXIS_C
	if(parser_word0 & GCODE_WORD_C)
    {
    	return STATUS_UNDEFINED_AXIS;
	}	
	#endif
	
	if(cnc_get_exec_state(EXEC_JOG))// if is jog emulates a G1 motion code
	{
		//if any motion is declared the jog is invalid
		if((parser_group0 & GCODE_GROUP_MOTION))
    	{
    		return STATUS_INVALID_JOG_COMMAND;
    	}
    	
    	//emulates G1
    	new_state->groups.motion = 1;
    	SETFLAG(parser_group0,GCODE_GROUP_MOTION);
	}

    //RS274NGC v3 - 3.5 G Codes
    //group 0 - non modal (incomplete)
    //TODO
    //4 Dwell
    if((parser_group1 & GCODE_GROUP_NONMODAL))
    {
    	switch(new_state->groups.nonmodal)
    	{
    		case 1:
    			//G10
    			//if no P or L is present
    			if(!(parser_word1 & GCODE_WORD_P) || !(parser_word1 & GCODE_WORD_L))
    			{
    				return STATUS_GCODE_VALUE_WORD_MISSING;
				}
				//L is not 2
				if(new_state->words.l != 2)
				{
					return STATUS_GCODE_UNSUPPORTED_COMMAND;
				}
				//P is not between 1 and N� of coord systems
				if(new_state->words.p<1 || new_state->words.p>COORD_SYS_COUNT)
				{
					return STATUS_GCODE_UNSUPPORTED_COORD_SYS;
				}
				break;
			case 5:
    			//G53
    			//if no G0 or G1 not active
    			if(new_state->groups.motion > 1)
    			{
    				return STATUS_GCODE_G53_INVALID_MOTION_MODE;
				}
				break;
		}
	}
	
    //group 1 - motion (incomplete)
    //TODO
    //38.2 probing
    //81...89 Canned cycles
    if((parser_group0 & GCODE_GROUP_MOTION))
    {
		switch(new_state->groups.motion)
		{
			case 0:
			case 1:
				if(!CHECKFLAG(parser_word0 , GCODE_ALL_AXIS))
				{
					return STATUS_GCODE_NO_AXIS_WORDS;
				}
				break;
			case 2:
			case 3:
				if(parser_word1 & GCODE_WORD_R) //arc in radius format
				{
					switch(new_state->groups.plane)
					{
						case 0:
							if(!CHECKFLAG(parser_word0,GCODE_XYPLANE_AXIS))
							{
								return STATUS_GCODE_NO_AXIS_WORDS_IN_PLANE;
							}
							
							if((parser_last_pos[AXIS_X] == new_state->words.xyzabc[AXIS_X]) && (parser_last_pos[AXIS_Y] == new_state->words.xyzabc[AXIS_Y]))
							{
								return STATUS_GCODE_INVALID_TARGET;
							}
							break;
						case 1:
							if(!CHECKFLAG(parser_word0,GCODE_XZPLANE_AXIS))
							{
								return STATUS_GCODE_NO_AXIS_WORDS_IN_PLANE;
							}
							
							if((parser_last_pos[AXIS_X] == new_state->words.xyzabc[AXIS_X]) && (parser_last_pos[AXIS_Z] == new_state->words.xyzabc[AXIS_Z]))
							{
								return STATUS_GCODE_INVALID_TARGET;
							}
							break;
						case 2:
							if(!CHECKFLAG(parser_word0,GCODE_YZPLANE_AXIS))
							{
								return STATUS_GCODE_NO_AXIS_WORDS_IN_PLANE;
							}
							
							if((parser_last_pos[AXIS_Y] == new_state->words.xyzabc[AXIS_Y]) && (parser_last_pos[AXIS_Z] == new_state->words.xyzabc[AXIS_Z]))
							{
								return STATUS_GCODE_INVALID_TARGET;
							}
							break;
					}
				}
				else
				{
					switch(new_state->groups.plane)
					{
						case 0:
							if(!CHECKFLAG(parser_word0,GCODE_XYPLANE_AXIS))
							{
								return STATUS_GCODE_NO_AXIS_WORDS_IN_PLANE;
							}
							
							if(!CHECKFLAG(parser_word1,GCODE_XYPLANE_AXIS))
							{
								return STATUS_GCODE_NO_OFFSETS_IN_PLANE;
							}
							break;
						case 1:
							if(!CHECKFLAG(parser_word0,GCODE_XZPLANE_AXIS))
							{
								return STATUS_GCODE_NO_AXIS_WORDS_IN_PLANE;
							}
							
							if(!CHECKFLAG(parser_word1,GCODE_XZPLANE_AXIS))
							{
								return STATUS_GCODE_NO_OFFSETS_IN_PLANE;
							}
							break;
						case 2:
							if(!CHECKFLAG(parser_word0,GCODE_YZPLANE_AXIS))
							{
								return STATUS_GCODE_NO_AXIS_WORDS_IN_PLANE;
							}
							
							if(!CHECKFLAG(parser_word1,GCODE_YZPLANE_AXIS))
							{
								return STATUS_GCODE_NO_OFFSETS_IN_PLANE;
							}
							break;
					}
				}
				break;
			case 4: //G38.2 (not implements yet)
				break;
			case 5: //G80 and 
				if(CHECKFLAG(parser_word0,GCODE_ALL_AXIS))
				{
					return STATUS_GCODE_AXIS_WORDS_EXIST;
				}
				break;
			default: //G81..G89 canned cycles (not implemented yet)
				break;
		}
		
		if((new_state->groups.motion >= 1 && new_state->groups.motion <= 3) && new_state->words.f == 0)
		{
			return STATUS_GCODE_UNDEFINED_FEED_RATE;
		}
	}
    //group 2 - plane selection (nothing to be checked)
    //group 3 - distance mode (nothing to be checked)
    //group 5 - feed rate mode (not implemented yet)
    
    //group 6 - units (nothing to be checked)
    //group 7 - cutter radius compensation (not implemented yet)
    //group 8 - tool length offset (not implemented yet)
    //group 10 - return mode in canned cycles (not implemented yet)
	//group 12 - coordinate system selection (not implemented yet)
	//group 13 - path control mode (nothing to be checked)
	
	//RS274NGC v3 - 3.6 Input M Codes
	//group 4 - (not implemented yet)
	//group 6 - (not implemented yet)
	//group 7 - (nothing to be checked)
	//group 8 - (not implemented yet)
	//group 9 - (not implemented yet)


	//RS274NGC v3 - 3.7 Other Input Codes
	//Words S and T must be positive
	if(new_state->words.s < 0 || new_state->words.t < 0)
    {
        return STATUS_NEGATIVE_VALUE;
	}
	
	if(new_state->words.t > g_settings.tool_count)
	{
		return STATUS_INVALID_TOOL;
	}

	return STATUS_OK;
}

/*
	STEP 3
	Executes the command
		Follows the RS274NGC v3 - 3.8 Order of Execution
	All coordinates are converted to machine absolute coordinates before sent to the motion controller
*/
static uint8_t parser_exec_command(parser_state_t *new_state)
{	
	float axis[AXIS_COUNT];
	float feed = new_state->words.f;
	uint8_t spindle = 0;
	//plane selection
	uint8_t a = 0;
	uint8_t b = 0;
	uint8_t offset_a = 0;
	uint8_t offset_b = 0;
	float radius;
	//pointer to planner position
	float planner_last_pos[AXIS_COUNT];
	
	planner_get_position(planner_last_pos);
	
	//RS274NGC v3 - 3.8 Order of Execution
	//1. comment (ignored - already filtered)
	//2. set feed mode (not implemented yet)
	//3. set feed rate (uCNC works in units per second and not per minute)
	if(CHECKFLAG(parser_word0, GCODE_WORD_F))
	{
		if(new_state->groups.feedrate_mode != 0)
		{
			new_state->words.f *= MIN_SEC_MULT;
		}
	}
		
	//4. set spindle speed
	float spindle_perc = MAX(new_state->words.s, g_settings.spindle_min_rpm)/g_settings.spindle_max_rpm;
	spindle = (uint8_t)roundf(255 * spindle_perc);
	
	//5. select tool (not implemented yet)
	//6. change tool (not implemented yet)
	//7. spindle on/off (not implemented yet)
	switch(new_state->groups.spindle_turning)
	{
		case 0:
			mc_spindle(spindle, false);
			break;
		case 1:
			mc_spindle(spindle, true);
			break;
		case 2:
			mc_spindle(0, true);
			break;
	}
	
	//8. coolant on/off
	mc_coolant(new_state->groups.coolant);

	//9. overrides (not implemented)
	//10. dwell (not implemented)
	if(CHECKFLAG(parser_group1, GCODE_GROUP_NONMODAL) && !new_state->groups.nonmodal)
	{
		mc_dwell((uint16_t)(new_state->words.p * 1000));
	}
	
	//11. set active plane (G17, G18, G19)
	switch(new_state->groups.plane)
	{
		case 0:
			a = AXIS_X;
			b = AXIS_Y;
			offset_a = 0;
			offset_b = 1;
			break;
		case 1:
			a = AXIS_X;
			b = AXIS_Z;
			offset_a = 0;
			offset_b = 2;
			break;
		case 2:
			a = AXIS_Y;
			b = AXIS_Z;
			offset_a = 1;
			offset_b = 2;
			break;
	}
	
	//12. set length units (G20, G21).
	if(new_state->groups.units == 0) //all internal state variables must be converted to mm
	{
		for(uint8_t i =  AXIS_COUNT; i != 0;)
		{
			i--;
			new_state->words.xyzabc[i] *= 25.4f;
		}
		
		//check if any i, j or k words were used
		if(CHECKFLAG(parser_word1, GCODE_MAIN_AXIS))
		{
			for(uint8_t i =  3; i != 0;)
			{
				i--;
				new_state->words.ijk[i] *= 25.4f;
			}
		}
		
		//if new feed is defined (normal feed mode) convert o mm
		if(CHECKFLAG(parser_word0, GCODE_WORD_F) && new_state->groups.feedrate_mode != 0)
		{
			new_state->words.f *= 25.4f;
		}

		if(CHECKFLAG(parser_word1, GCODE_WORD_R))
		{
			new_state->words.r *= 25.4f;
		}
	}

	//13. cutter radius compensation on or off (G40, G41, G42) (not implemented yet)
	//14. cutter length compensation on or off (G43, G49) (not implemented yet)
	//15. coordinate system selection (G54, G55, G56, G57, G58, G59, G59.1, G59.2, G59.3) (OK nothing to be done)

	//16. set path control mode (G61, G61.1, G64) (not implemented yet)
	
	//17. set distance mode (G90, G91) (OK nothing to be done)

	//18. set retract mode (G98, G99)  (not implemented yet)
	//19. home (G28, G30) or change coordinate system data (G10) or set axis offsets (G92, G92.1, G92.2, G92.3)
	//	or also modifies target if G53 is active. These are executed after calculating intemediate targets (G28 ad G30)
	if(new_state->groups.nonmodal != 5)//if not modified by G53
	{
		memset(&axis, 0, sizeof(axis));	
	
    	if((new_state->groups.distance_mode==0)) 
		{
			for(uint8_t i =  AXIS_COUNT; i != 0;)
			{
				i--;
				axis[i] = new_state->words.xyzabc[i] + parser_parameters.coord_sys[new_state->groups.coord_system][i] + parser_offset_pos[i];
			}
		}
		else
		{
			for(uint8_t i =  AXIS_COUNT; i != 0;)
			{
				i--;
				axis[i] = planner_last_pos[i] + new_state->words.xyzabc[i];
			}
		}

		//for all not explicitly declared axis retain their position
		#ifdef AXIS_X
		if(!CHECKFLAG(parser_word0, GCODE_WORD_X))
		{
			axis[AXIS_X] = planner_last_pos[AXIS_X];
		}
		#endif
		#ifdef AXIS_Y
		if(!CHECKFLAG(parser_word0, GCODE_WORD_Y))
		{
			axis[AXIS_Y] = planner_last_pos[AXIS_Y];
		}
		#endif
		#ifdef AXIS_Z
		if(!CHECKFLAG(parser_word0, GCODE_WORD_Z))
		{
			axis[AXIS_Z] = planner_last_pos[AXIS_Z];
		}
		#endif
		#ifdef AXIS_A
		if(!CHECKFLAG(parser_word0, GCODE_WORD_A))
		{
			axis[AXIS_A] = planner_last_pos[AXIS_A];
		}
		#endif
		#ifdef AXIS_B
		if(!CHECKFLAG(parser_word0, GCODE_WORD_B))
		{
			axis[AXIS_B] = planner_last_pos[AXIS_B];
		}
		#endif
		#ifdef AXIS_C
		if(!CHECKFLAG(parser_word0, GCODE_WORD_C))
		{
			axis[AXIS_C] = planner_last_pos[AXIS_C];
		}
		#endif
	}
	else //if modified by G53
	{
		memcpy(&axis, &new_state->words.xyzabc, sizeof(axis));
	}
	
	if(new_state->groups.nonmodal != 0)
	{
		uint8_t index = 0;
		uint8_t error = 0;
		switch(new_state->groups.nonmodal)
		{
			case 1: //G10
				index = (uint8_t)new_state->words.p;
				index--;
				for(uint8_t i =  AXIS_COUNT; i != 0;)
				{
					i--;
					parser_parameters.coord_sys[index][i] = new_state->words.xyzabc[i];
				}
				return STATUS_OK;
			case 2: //G28
				error = mc_line((float*)&axis, feed);
				if(error)
				{
					return error;
				}
				return mc_line((float*)&parser_parameters.g28home, feed);
			case 3: //G28
				error = mc_line((float*)&axis, feed);
				if(error)
				{
					return error;
				}
				return mc_line((float*)&parser_parameters.g30home, feed);
			case 9: //G92
				for(uint8_t i =  AXIS_COUNT; i != 0;)
				{
					i--;
					float wpos = planner_last_pos[i] - parser_parameters.coord_sys[new_state->groups.coord_system][i] - parser_offset_pos[i];
					parser_offset_pos[i] += wpos - new_state->words.xyzabc[i];
				}
				
				memcpy(&parser_parameters.g92offset, &parser_offset_pos, sizeof(parser_offset_pos));
				return STATUS_OK;
			case 10: //G92.1
				memset(&parser_parameters.g92offset,0, sizeof(parser_parameters.g92offset));
				memset(&parser_offset_pos,0, sizeof(parser_offset_pos));
				return STATUS_OK;
			case 11: //G92.2
				memset(&parser_offset_pos,0, sizeof(parser_offset_pos));
				return STATUS_OK;
			case 12: //G92.3
				memcpy(&parser_offset_pos, &parser_parameters.g92offset, sizeof(parser_offset_pos));
				return STATUS_OK;
		}
	}
	
	float x, y;
	//20. perform motion (G0 to G3, G80 to G89), as modified (possibly) by G53.
	//	incomplete (canned cycles not supported)
	if(!CHECKFLAG(parser_word0 , GCODE_ALL_AXIS)) //if no axis was issued then no motion to be done
	{
		return STATUS_OK;
	}
	
	feed = MIN(new_state->words.f,parser_max_feed_rate);
	
	switch(new_state->groups.motion)
	{
		case 0:
			feed = parser_max_feed_rate;
			return mc_line(axis, feed);
			break;
		case 1:
			if(feed == 0)
			{
				return STATUS_FEED_NOT_SET;
			}
			return mc_line(axis, feed);
			break;
		case 2:
		case 3:
			//has calculated in grbl
			x = axis[a] - planner_last_pos[a];
			y = axis[b] - planner_last_pos[b];
				
			//radius mode
			if(CHECKFLAG(parser_word1, GCODE_WORD_R))
			{
				if(x == 0 && y == 0)
				{
					return STATUS_GCODE_INVALID_TARGET;
				}

				float x_sqr = x * x;
				float y_sqr = y * y;
				float c_factor = 4.0 * new_state->words.r * new_state->words.r - x_sqr - y_sqr;
			
            	if (c_factor < 0) 
				{
					return STATUS_GCODE_ARC_RADIUS_ERROR;
				}
				
				c_factor = -sqrt((c_factor)/(x_sqr + y_sqr));
				
				if (new_state->groups.motion == 3)
				{
					c_factor = -c_factor;
				}
				
				if (new_state->words.r < 0)
				{
                	c_factor = -c_factor;
                	new_state->words.r = -new_state->words.r; // Finished with r. Set to positive for mc_arc
            	}
            	
	            // Complete the operation by calculating the actual center of the arc
	            new_state->words.ijk[offset_a] = 0.5 * (x - (y * c_factor));
	            new_state->words.ijk[offset_b] = 0.5 * (y + (x * c_factor));
	            radius = new_state->words.r;
			}			
			else //offset mode
			{
				//calculate radius and check if center is within tolerances
				radius = sqrtf(new_state->words.ijk[offset_a] * new_state->words.ijk[offset_a] + new_state->words.ijk[offset_b] * new_state->words.ijk[offset_b]);
				float x1 = x - new_state->words.ijk[offset_a];
				float y1 = y - new_state->words.ijk[offset_b];
				float r1 = sqrt(x1 * x1 + y1 * y1);
				if(fabs(radius - r1) > 0.002) //error must not exceed 0.002mm
				{
					return STATUS_GCODE_INVALID_TARGET;
				}
			}

			return mc_arc(axis, new_state->words.ijk[offset_a], new_state->words.ijk[offset_b], radius, new_state->groups.plane, (new_state->groups.motion==2), feed);
			break;
	}
	
	//stop (M0, M1, M2, M30, M60) (not implemented yet).
	switch(new_state->groups.stopping)
	{
		case 0: //M0
			break;
		case 1: //M1
			break;
		case 2: //M2
		case 3: //M30
			//reset to initial states
			new_state->groups.coord_system = 0;
			memset(&parser_offset_pos,0, sizeof(parser_offset_pos));
			new_state->groups.plane = 0;
			new_state->groups.distance_mode = 1;
			new_state->groups.feedrate_mode = 1;
			new_state->groups.feed_speed_override = 1;
			new_state->groups.cutter_radius_compensation = 0;
			new_state->groups.spindle_turning = 2;
			new_state->groups.motion = 1;
			new_state->groups.coolant = 2;			
			break;
		case 6:	//M60
			break;
	}
	return STATUS_OK;
}

/*
	Initializes the gcode parser 
*/
bool parser_init()
{
	parser_checkmode = false;
	memset(&parser_state, 0, sizeof(parser_state_t));
	memset(&parser_parameters, 0, sizeof(parser_parameters_t));
	
	parser_state.groups.coord_system = 0; 						//G54
	memset(&parser_offset_pos,0, sizeof(parser_offset_pos));	//G92.2
	parser_state.groups.plane = 0;								//G17
	parser_state.groups.distance_mode = 1;						//G91
	parser_state.groups.feedrate_mode = 1;						//G94
	parser_state.groups.feed_speed_override = 0;				//M48
	parser_state.groups.cutter_radius_compensation = 0;			//M40
	parser_state.groups.spindle_turning = 2;					//M5
	parser_state.groups.motion = 1;								//G1
	parser_state.groups.coolant = 2;							//M9
	parser_state.groups.units = 1;								//G21

	for(uint8_t i = 0; i < AXIS_COUNT; i++)
	{
		parser_max_feed_rate = MAX(parser_max_feed_rate, g_settings.max_feed_rate[i]);
	}
	
	parser_max_feed_rate *= MIN_SEC_MULT;
	
	return settings_load(SETTINGS_PARSER_PARAMETERS_ADDRESS_OFFSET, (uint8_t*)&parser_parameters, sizeof(parser_parameters_t));
}

bool parser_is_ready()
{
	return !planner_buffer_is_full();
}

/*
	Parse the next gcode line available in the buffer and send it to the motion controller
*/
uint8_t parser_gcode_command()
{
	uint8_t result = 0;
	//initializes new state
	parser_state_t next_state = {};
	//next state will be the same as previous except for nonmodal group (is set with 0)
	memcpy(&next_state.groups, &parser_state.groups, sizeof(parser_groups_t));
	//also must preserve feedrate, tool and spindle
	next_state.words.f = parser_state.words.f;
	next_state.words.t = parser_state.words.t;
	next_state.words.s = parser_state.words.s;
    next_state.groups.nonmodal = 0;
    
    //fetch command
	result = parser_fetch_command(&next_state);
	if(result != STATUS_OK)
	{
		return result;
	}
	
	//validates command
	result = parser_validate_command(&next_state);
	if(result != STATUS_OK)
	{
		return result;
	}
	
	//executes command
	result = parser_exec_command(&next_state);
	if(result != STATUS_OK)
	{
		return result;
	}
	
	//if is jog motion state is not preserved
	if(!cnc_get_exec_state(EXEC_JOG))
	{
		//if everything went ok updates the parser modal groups and position
		memcpy(&parser_state.groups, &next_state.groups, sizeof(parser_groups_t));
		parser_state.words.f = next_state.words.f;
		parser_state.words.t = next_state.words.t;
		parser_state.words.s = next_state.words.s;
		
	}

	return result;
}

uint8_t parser_grbl_command()
{
	//if not IDLE
	if(cnc_get_exec_state(EXEC_RUN))
	{
		return STATUS_IDLE_ERROR;
	}

	char c = serial_peek();
	
	switch(c)
	{
		/*case '\r':
			serial_print_string(MSG_HELP);
			return STATUS_OK;*/
		case '$':
			serial_getc();
			if(serial_getc() != '\n')
			{
				return STATUS_INVALID_STATEMENT;
			}
			
			protocol_send_gcode_settings();
			return STATUS_OK;
		case '#':
			serial_getc();
			if(serial_getc() != '\n')
			{
				return STATUS_INVALID_STATEMENT;
			}
			
			protocol_send_gcode_coordsys();
			return STATUS_OK;
		case 'H':
			serial_getc();
			if(serial_getc() != '\n')
			{
				return STATUS_INVALID_STATEMENT;
			}
			if(!g_settings.homing_enabled)
			{
				return STATUS_SETTING_DISABLED;
			}
			
			if(dio_get_controls(ESTOP_MASK | SAFETY_DOOR_MASK))
			{
				return STATUS_CHECK_DOOR;
			}
			
			cnc_home();
			return STATUS_OK;
		case 'X':
			serial_getc();
			if(serial_getc() != '\n')
			{
				return STATUS_INVALID_STATEMENT;
			}
			
			if(dio_get_controls(ESTOP_MASK | SAFETY_DOOR_MASK))
			{
				return STATUS_CHECK_DOOR;
			}
			cnc_unlock();
			return STATUS_OK;
		case 'G':
			serial_getc();
			if(serial_getc() != '\n')
			{
				return STATUS_INVALID_STATEMENT;
			}
			
			protocol_send_gcode_modes();
			return STATUS_OK;
		case 'R':
			serial_getc();
			if(serial_getc() != 'S')
			{
				return STATUS_INVALID_STATEMENT;
			}
			if(serial_getc() != 'T')
			{
				return STATUS_INVALID_STATEMENT;
			}
			if(serial_getc() != '=')
			{
				return STATUS_INVALID_STATEMENT;
			}
			c = serial_getc();
			switch(c)
			{
				case '$':
					settings_reset();
					break;
				case '#':
					parser_parameters_reset();
					break;
				case '*':
					settings_reset();
					parser_parameters_reset();
					break;
				default:
					return STATUS_INVALID_STATEMENT;
					break;
			}
			
			return STATUS_OK;
		case 'J': //jog command
			/*
				The jog command is parsed like an emulated G1 command without changing the parser state
				1. Fetch the command
				2. Validates the command (in jog mode)
				3. Executes it as a G1 command
			*/
			serial_getc();
			if(serial_getc() != '=')
			{
				return STATUS_INVALID_JOG_COMMAND;
			}
			
			cnc_set_exec_state(EXEC_JOG);
			return parser_gcode_command();
		case 'C':
			serial_getc();
			if(serial_getc() != '\n')
			{
				return STATUS_INVALID_STATEMENT;
			}

			//toggles motion control check mode
			if(mc_toogle_checkmode())
			{
				protocol_send_string(MSG_FEEDBACK_4);
			}
			else
			{
				cnc_exec_rt_command(RT_CMD_RESET);
				protocol_send_string(MSG_FEEDBACK_5);
			}
			return STATUS_OK;	
		default:
			if(c >= '0' &&  c <= '9') //settings
			{
				float val = 0;
				uint8_t setting_num = 0;
				bool isinteger = false;
				
				parser_get_float(&val, &isinteger);
				
				if(!isinteger || val > 255 || val < 0)
				{
					return STATUS_INVALID_STATEMENT;
				}
				
				setting_num = (uint8_t)val;
				//eat '='
				if(serial_getc() != '=')
				{
					return STATUS_INVALID_STATEMENT;
				}
				
				val = 0;
				isinteger = false;
				parser_get_float(&val, &isinteger);
				//eat '='
				if(serial_getc() != '\n')
				{
					return STATUS_INVALID_STATEMENT;
				}

				return settings_change(setting_num, val);
			}
			return STATUS_INVALID_STATEMENT;
	}

	return STATUS_INVALID_STATEMENT;
}

void parser_get_modes(uint8_t* modalgroups, uint16_t* feed, uint16_t* spindle)
{
	modalgroups[0] = parser_state.groups.motion;
	modalgroups[1] = parser_state.groups.plane + 17;
	modalgroups[2] = parser_state.groups.distance_mode + 90;
	modalgroups[3] = parser_state.groups.units + 20;
	modalgroups[4] = parser_state.groups.coord_system + 54;
	modalgroups[5] = parser_state.groups.spindle_turning + 3;
	modalgroups[6] = parser_state.groups.coolant + 7;
	modalgroups[7] = parser_state.groups.feed_speed_override + 48;
	modalgroups[8] = parser_state.words.t;
	*feed = (uint16_t)parser_state.words.f;
	*spindle = (uint16_t)parser_state.words.s;
}

float* parser_get_coordsys(uint8_t system_num)
{
	switch(system_num)
	{
		case 28:
			return (float*)&parser_parameters.g28home;
		case 30:
			return (float*)&parser_parameters.g30home;
		case 92:
			return (float*)&parser_parameters.g92offset;
		default:
			return (float*)&parser_parameters.coord_sys[system_num];
	}
}

void parser_parameters_reset()
{
	//erase all parameters
	memset(&parser_parameters, 0, sizeof(parser_parameters_t));
	settings_save(SETTINGS_PARSER_PARAMETERS_ADDRESS_OFFSET, (const uint8_t*)&parser_parameters, sizeof(parser_parameters_t));
}

void parser_get_wco(float* axis)
{
	for(uint8_t i = 0; i < AXIS_COUNT;i++)
	{
		axis[i] = parser_parameters.g92offset[i] + parser_parameters.coord_sys[parser_state.groups.coord_system][i];
	}
}
