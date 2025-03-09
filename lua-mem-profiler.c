#include <lua.h>
#include <lauxlib.h>
#include <string.h>
#include <stdlib.h>
#include <lstate.h>
#include <lobject.h>

#define MAX_LEVEL 3

struct mem_profile_log {
    int line;                 
    char source[LUA_IDSIZE];  
    size_t memory_used;      
    size_t memory_delta;   
};

struct mem_profile_count {
    int total;               
    int index;          
};

struct mem_source_info {
    char source[LUA_IDSIZE];
    struct mem_line_info* lines;
    int line_count;
};

struct mem_line_info {
    int line;
    size_t memory_used;
    size_t memory_delta;
    char source[LUA_IDSIZE];
};

static void
mem_profile_hook(lua_State *L, lua_Debug *ar) {
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, L) != LUA_TUSERDATA) {
        lua_pop(L, 1);
        return;
    }
    struct mem_profile_count * p = lua_touserdata(L, -1);
    lua_pop(L, 1);
    struct mem_profile_log * log = (struct mem_profile_log *)(p+1);
    
    int index = p->index++;
    while(index >= p->total) {
        index -= p->total;
    }
    
    if (lua_getinfo(L, "SL", ar) != 0) {
        log[index].line = ar->currentline;
        strcpy(log[index].source, ar->short_src);
    }else{
        log[index].line = 1;
        strcpy(log[index].source, "[unknown]");
    }
    
    size_t current_memory = lua_gc(L, LUA_GCCOUNT, 0) * 1024 
                         + lua_gc(L, LUA_GCCOUNTB, 0);
    
    if (index > 0) {
        log[index].memory_delta = current_memory - log[index-1].memory_used;
    }
    log[index].memory_used = current_memory;
}

static int
lstart(lua_State *L) {
    lua_State *cL = L;
    if (lua_isthread(L, 1)) {
        cL = lua_tothread(L, 1);
    }
    
    int sample_count = luaL_optinteger(L, 2, 1000);  
    
    struct mem_profile_count * p = lua_newuserdata(L, 
        sizeof(struct mem_profile_count) + sample_count * sizeof(struct mem_profile_log));
    p->total = sample_count;
    p->index = 0;
    
    lua_pushvalue(L, -1);
    lua_rawsetp(L, LUA_REGISTRYINDEX, cL);
    lua_sethook(cL, mem_profile_hook, LUA_MASKLINE, 0);
    
    return 0;
}

static int
lstop(lua_State *L) {
    lua_State *cL = L;
    if (lua_isthread(L, 1)) {
        cL = lua_tothread(L, 1);
    }
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, cL) != LUA_TNIL) {
        lua_pushnil(L);
        lua_rawsetp(L, LUA_REGISTRYINDEX, cL);
        lua_sethook(cL, NULL, 0, 0);
    } else {
        return luaL_error(L, "memory profiler not started");
    }
    return 0;
}

static int
linfo(lua_State *L) {
    lua_State *cL = L;
    if (lua_isthread(L, 1)) {
        cL = lua_tothread(L, 1);
    }
    
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, cL) != LUA_TUSERDATA) {
        return luaL_error(L, "memory profiler not started");
    }
    struct mem_profile_count * p = lua_touserdata(L, -1);
    struct mem_profile_log * log = (struct mem_profile_log *)(p+1);
    
    lua_newtable(L);  
    int n = (p->index > p->total) ? p->total : p->index;
    
    for (int i = 0; i < n; i++) {
        luaL_getsubtable(L, -1, log[i].source);
        lua_rawgeti(L, -1, log[i].line);
        
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_createtable(L, 0, 2);
        }
        
        lua_pushinteger(L, log[i].memory_used);
        lua_setfield(L, -2, "memory");
        
        lua_pushinteger(L, log[i].memory_delta);
        lua_setfield(L, -2, "memory_delta");
        
        lua_rawseti(L, -2, log[i].line);
        lua_pop(L, 1);
    }
    
    lua_pushinteger(L, p->index); 
    return 2;
}

static void trim_string(char* str) {
    char* start = str;
    char* end = str + strlen(str) - 1;
    
    while(end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    
    *(end + 1) = '\0';
    
    if(start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

static int
lprint(lua_State *L) {
    lua_State *cL = L;
    if (lua_isthread(L, 1)) {
        cL = lua_tothread(L, 1);
    }
    
    if (lua_rawgetp(L, LUA_REGISTRYINDEX, cL) != LUA_TUSERDATA) {
        return luaL_error(L, "thread profiler not begin");
    }
    
    struct mem_profile_count * p = lua_touserdata(L, -1);
    struct mem_profile_log * log = (struct mem_profile_log *)(p+1);
    
    int n = (p->index > p->total) ? p->total : p->index;

    struct mem_source_info* source_info = NULL;
    int source_count = 0;
    
    for (int i = 0; i < n; i++) {
        int source_found = 0;
        
        for (int j = 0; j < source_count; j++) {
            if (strcmp(source_info[j].source, log[i].source) == 0) {
                int line_found = 0;
                for (int k = 0; k < source_info[j].line_count; k++) {
                    if (source_info[j].lines[k].line == log[i].line) {
                        source_info[j].lines[k].memory_used = log[i].memory_used;
                        source_info[j].lines[k].memory_delta = log[i].memory_delta;
                        line_found = 1;
                        break;
                    }
                }
                
                if (!line_found) {
                    int new_idx = source_info[j].line_count++;
                    source_info[j].lines = realloc(source_info[j].lines, 
                        source_info[j].line_count * sizeof(struct mem_line_info));
                    source_info[j].lines[new_idx].line = log[i].line;
                    source_info[j].lines[new_idx].memory_used = log[i].memory_used;
                    source_info[j].lines[new_idx].memory_delta = log[i].memory_delta;
                    strcpy(source_info[j].lines[new_idx].source, log[i].source);
                }
                source_found = 1;
                break;
            }
        }
        
        if (!source_found) {
            source_info = realloc(source_info, (source_count + 1) * sizeof(struct mem_source_info));
            strcpy(source_info[source_count].source, log[i].source);
            source_info[source_count].line_count = 1;
            source_info[source_count].lines = malloc(sizeof(struct mem_line_info));
            source_info[source_count].lines[0].line = log[i].line;
            source_info[source_count].lines[0].memory_used = log[i].memory_used;
            source_info[source_count].lines[0].memory_delta = log[i].memory_delta;
            strcpy(source_info[source_count].lines[0].source, log[i].source);
            source_count++;
        }
    }
    for (int i = 0; i < source_count; i++) {
        for (int j = 0; j < source_info[i].line_count - 1; j++) {
            for (int k = 0; k < source_info[i].line_count - j - 1; k++) {
                if (source_info[i].lines[k].line > source_info[i].lines[k + 1].line) {
                    struct mem_line_info temp = source_info[i].lines[k];
                    source_info[i].lines[k] = source_info[i].lines[k + 1];
                    source_info[i].lines[k + 1] = temp;
                }
            }
        }
    }
    
    FILE *last_file = NULL;
    char *lines[10000] = {NULL};
    char last_source[LUA_IDSIZE] = "";
    
    printf("\n--------------------------mem profiler print start --------------------\n");
    
    for (int i = 0; i < source_count; i++) {
        if (strcmp(last_source, source_info[i].source) != 0) {
            if (last_file) {
                fclose(last_file);
                for (int j = 0; j < 10000; j++) {
                    if (lines[j]) {
                        free(lines[j]);
                        lines[j] = NULL;
                    }
                }
            }
            
            strcpy(last_source, source_info[i].source);
            last_file = fopen(source_info[i].source, "r");
            if (last_file) {
                char line[256];
                int line_num = 1;
                while (fgets(line, sizeof(line), last_file) && line_num < 10000) {
                    trim_string(line);  
                    if (strlen(line) > 0) {  
                        char* new_line = strdup(line);
                        if (new_line) {
                            lines[line_num] = new_line;
                        }
                    }
                    line_num++;
                }
            }
        }
        int lastLine = 0;
        for(int j = 0; j < source_info[i].line_count; j++) {
            struct mem_line_info* info = source_info[i].lines;
            if(lastLine != 0 && lastLine + 1 != info[j].line) {
                printf("...\n");
            }
            lastLine = info[j].line;
            if (last_file && lines[info[j].line]) {
                printf("[%s:%d]--> %s         --memory used: %zu KB, delta: %zd KB\n",
                    info[j].source,
                    info[j].line,
                    lines[info[j].line],
                    info[j].memory_used / 1024,
                    info[j].memory_delta / 1024);
            } else {
                printf("%s:%d             --memory used: %zu KB, delta: %zd KB\n",
                    info[j].source,
                    info[j].line,
                    info[j].memory_used / 1024,
                    info[j].memory_delta / 1024);
            }
            
        }
    }
    
    if (last_file) {
        fclose(last_file);
        for (int j = 0; j < 10000; j++) {
            if (lines[j]) {
                free(lines[j]);
            }
        }
    }
    for (int i = 0; i < source_count; i++) {
        free(source_info[i].lines);
    }
    free(source_info);

    printf("\n--------------------------mem profiler print end --------------------\n");
    return 0;
}

size_t estimate_constant_table_size(const Proto *p) {
    size_t total_size = 0;
    for (int i = 0; i < p->sizek; i++) {
        TValue *constant = &p->k[i];
        switch (ttype(constant)) {
            case LUA_TNUMBER:
                total_size += sizeof(lua_Number);
                break;
            case LUA_TSTRING:
                total_size += sizeof(TString) + tsslen(tsvalue(constant));
                break;
            case LUA_TBOOLEAN:
                total_size += sizeof(int);  
                break;
            default:
                break;
        }
    }
    for (int i = 0; i < p->sizep; i++) {
        total_size = total_size + estimate_constant_table_size(p->p[i]);
    }
    return total_size;
}

static void print_sizek_info(const Proto *p) {
    printf("------> Source: %s\n", getstr(p->source));
    printf("total sizek %ld\n", estimate_constant_table_size(p));
    printf("------>\n");
}

static int
lsizekinfo(lua_State *L) {
    lua_State * co = L;
    if (lua_isthread(L, 1)) {
        co = lua_tothread (L, 1);
    }
    lua_Debug ar;
    int index = 0;
    int level = 0;
    do {
        if (!lua_getstack(co, level, &ar))
            break;
        lua_getinfo(co, "f", &ar);
        level++;
        LClosure *cl = clLvalue(s2v(ar.i_ci->func.p));
        if (cl == NULL || !ttisLclosure(s2v(ar.i_ci->func.p))) {
            continue;
        }
    
        Proto *p = cl->p;
        if (p == NULL) {
            continue;
        }
        print_sizek_info(p);
    } while (index < MAX_LEVEL);


    return 0;
}

LUAMOD_API int
luaopen_mprofiler(lua_State *L) {
    luaL_checkversion(L);
    luaL_Reg l[] = {
        { "start", lstart },
        { "stop", lstop },
        { "info", linfo },
        { "print", lprint },
        { "sizekinfo", lsizekinfo },
        { NULL, NULL },
    };
    luaL_newlib(L, l);
    return 1;
}