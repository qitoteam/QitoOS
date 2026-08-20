/*
 * QitoOS - network, executable, icon, library and package commands
 */

#include <kernel/shell.h>
#include <kernel/net.h>
#include <kernel/http.h>
#include <kernel/qtx.h>
#include <kernel/qti.h>
#include <kernel/qdl.h>
#include <kernel/font.h>
#include <kernel/fs.h>
#include <kernel/string.h>
#include <kernel/printf.h>
#include <kernel/mm.h>
#include <kernel/time.h>
#include <kernel/qtpkg.h>

/* --- networking -------------------------------------------------------- */

static int cmd_fetch(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "usage: fetch <url> [-o file] [-H]");
        shell_printf(sh, "  -o  write body to file\n");
        shell_printf(sh, "  -H  show headers\n");
        return 1;
    }
    const char *url = argv[1];
    const char *dest = NULL;
    bool_t show_headers = false;
    for (int i=2;i<argc;i++) {
        if (strcmp(argv[i],"-o")==0 && i+1<argc) dest=argv[++i];
        else if (strcmp(argv[i],"-H")==0) show_headers=true;
    }
    struct http_response resp;
    uint64_t start=time_uptime_ms();
    if (http_get(url,&resp)!=0) { shell_error(sh,"fetch: %s",resp.error); return 1; }
    uint64_t elapsed=time_uptime_ms()-start;
    shell_color(sh,"\033[96m");
    shell_printf(sh,"%d %s",resp.status,http_status_text(resp.status));
    shell_reset_color(sh);
    shell_printf(sh,"  %llu bytes in %llu ms\n",(unsigned long long)resp.body_length,(unsigned long long)elapsed);
    if (show_headers) shell_printf(sh,"\n%s\n\n",resp.headers);
    if (dest) {
        char resolved[FS_PATH_MAX];
        shell_resolve(sh,dest,resolved,sizeof(resolved));
        if (fs_write_file(resolved,resp.body,resp.body_length)==0) {
            shell_printf(sh,"wrote %llu bytes to %s\n",(unsigned long long)resp.body_length,resolved);
        } else { shell_error(sh,"cannot write %s",resolved); http_free(&resp); return 1; }
    } else {
        shell_write(sh,resp.body,resp.body_length);
        if (resp.body_length && resp.body[resp.body_length-1]!='\n') shell_printf(sh,"\n");
    }
    http_free(&resp);
    return 0;
}

static int cmd_lookup(struct shell *sh, int argc, char **argv)
{
    if (argc<2){ shell_error(sh,"usage: lookup <hostname>"); return 1; }
    char server[24];
    net_format_ip(net_dns_server(),server,sizeof(server));
    shell_printf(sh,"Resolver: %s\n",server);
    uint64_t start=time_uptime_ms();
    ipv4_addr_t addr;
    if (dns_resolve(argv[1],&addr,5000)!=0){ shell_error(sh,"lookup: no answer for %s",argv[1]); return 1; }
    char text[24];
    net_format_ip(addr,text,sizeof(text));
    shell_printf(sh,"%s has address %s (%llu ms)\n",argv[1],text,(unsigned long long)(time_uptime_ms()-start));
    return 0;
}

/* --- QTX --------------------------------------------------------------- */

static int cmd_qtx(struct shell *sh, int argc, char **argv)
{
    if (argc<2){ shell_printf(sh,"usage: qtx <info|verify|exports|run|list> [file]\n"); return 1; }
    if (strcmp(argv[1],"exports")==0){
        shell_printf(sh,"Kernel services available to QTX programs:\n\n");
        int count=qtx_export_count();
        for (int i=0;i<count;i++){
            shell_printf(sh,"  %-22s",qtx_export_name(i));
            if ((i%3)==2) shell_printf(sh,"\n");
        }
        if (count%3) shell_printf(sh,"\n");
        shell_printf(sh,"\n%d service(s)\n",count);
        return 0;
    }
    if (strcmp(argv[1],"list")==0){
        shell_printf(sh,"Scanning for .qtx files in /bin and /usr/bin...\n");
        const char *dirs[]={"/bin","/usr/bin","/user/bin",NULL};
        for (int d=0;dirs[d];d++){
            struct fs_node *dir=fs_lookup(dirs[d]);
            if (!dir) continue;
            struct fs_dirent de;
            for (int i=0;fs_readdir(dir,i,&de)==0;i++){
                size_t len=strlen(de.name);
                if (len<5||strcmp(de.name+len-4,".qtx")!=0) continue;
                char path[FS_PATH_MAX];
                snprintf(path,sizeof(path),"%s/%s",dirs[d],de.name);
                shell_printf(sh,"  %s\n",path);
            }
        }
        return 0;
    }
    if (argc<3){ shell_error(sh,"usage: qtx %s <file>",argv[1]); return 1; }
    char resolved[FS_PATH_MAX];
    shell_resolve(sh,argv[2],resolved,sizeof(resolved));

    if (strcmp(argv[1],"info")==0){
        struct qx_header hdr;
        if (qtx_probe(resolved,&hdr)!=0){ shell_error(sh,"qtx: %s not a QTX image",argv[2]); return 1; }
        shell_printf(sh,"%s\n",resolved);
        qtx_describe(&hdr,sh);
        return 0;
    }
    if (strcmp(argv[1],"verify")==0){
        struct fs_stat st;
        if (fs_stat(resolved,&st)!=0){ shell_error(sh,"qtx: no such file"); return 1; }
        void *buf=kmalloc(st.size);
        if (!buf){ shell_error(sh,"out of mem"); return 1; }
        size_t got=0; fs_read_file(resolved,buf,st.size,&got);
        const char *reason=NULL;
        int res=qtx_validate(buf,got,&reason);
        kfree(buf);
        if (res==0){ shell_color(sh,"\033[92m"); shell_printf(sh,"%s is valid QTX\n",argv[2]); shell_reset_color(sh); return 0; }
        shell_color(sh,"\033[91m"); shell_printf(sh,"%s invalid: %s\n",argv[2],reason?reason:"unknown"); shell_reset_color(sh); return 1;
    }
    if (strcmp(argv[1],"run")==0){
        struct qtx_image img;
        if (qtx_load(resolved,&img)!=0){ shell_error(sh,"qtx: could not load %s",argv[2]); return 1; }
        int pid=qtx_run(&img,argc-2,argv+2);
        if (pid<0){ shell_error(sh,"qtx: could not start"); qtx_unload(&img); return 1; }
        shell_printf(sh,"started '%s' as pid %d (Ring3 изолирован)\n",img.name,pid);
        return 0;
    }
    shell_error(sh,"qtx: unknown subcommand '%s'",argv[1]); return 1;
}

/* --- QDL --------------------------------------------------------------- */

static int cmd_qdl(struct shell *sh, int argc, char **argv)
{
    if (argc<2||strcmp(argv[1],"list")==0){
        shell_printf(sh,"Loaded QDLs: %d\n",qdl_loaded_count());
        for (int i=0;i<qdl_loaded_count();i++){
            const char *name=qdl_loaded_name(i);
            shell_printf(sh,"  %s\n",name?name:"?");
        }
        qdl_describe(sh);
        return 0;
    }
    if (strcmp(argv[1],"load")==0){
        if (argc<3){ shell_error(sh,"usage: qdl load <file.qdl>"); return 1; }
        char resolved[FS_PATH_MAX];
        shell_resolve(sh,argv[2],resolved,sizeof(resolved));
        if (qdl_load(resolved)==0){ shell_printf(sh,"loaded %s\n",resolved); return 0; }
        shell_error(sh,"qdl: failed to load %s",resolved); return 1;
    }
    if (strcmp(argv[1],"unload")==0){
        if (argc<3){ shell_error(sh,"usage: qdl unload <name>"); return 1; }
        if (qdl_unload(argv[2])==0){ shell_printf(sh,"unloaded %s\n",argv[2]); return 0; }
        shell_error(sh,"qdl: %s not loaded or in use",argv[2]); return 1;
    }
    if (strcmp(argv[1],"info")==0){
        if (argc<3){ shell_error(sh,"usage: qdl info <file>"); return 1; }
        char resolved[FS_PATH_MAX];
        shell_resolve(sh,argv[2],resolved,sizeof(resolved));
        struct qx_header hdr;
        if (qtx_probe(resolved,&hdr)!=0){ shell_error(sh,"qdl: not a QDL"); return 1; }
        shell_printf(sh,"%s\n",resolved);
        qtx_describe(&hdr,sh);
        return 0;
    }
    shell_error(sh,"qdl: unknown subcommand"); return 1;
}

/* --- QTI --------------------------------------------------------------- */

static int cmd_qti(struct shell *sh, int argc, char **argv)
{
    if (argc<2||strcmp(argv[1],"list")==0){
        int count=qti_registry_count();
        shell_printf(sh,"Loaded QTI icons (%d), default %d px:\n\n",count,QTI_DEFAULT_SIZE);
        for (int i=0;i<count;i++){
            const char *name=qti_registry_name(i);
            const struct qti_image *img=qti_get(name);
            shell_printf(sh,"  %-16s %dx%d\n",name,img?img->width:0,img?img->height:0);
        }
        shell_printf(sh,"\nSizes: 16,32,64,128,256 – default 64 (third). Use qti info <file>\n");
        return 0;
    }
    if (strcmp(argv[1],"info")==0){
        if (argc<3){ shell_error(sh,"usage: qti info <file>"); return 1; }
        char resolved[FS_PATH_MAX];
        shell_resolve(sh,argv[2],resolved,sizeof(resolved));
        char buffer[4096]; size_t got=0;
        if (fs_read_file(resolved,buffer,sizeof(buffer),&got)!=0){ shell_error(sh,"qti: cannot read"); return 1; }
        struct qti_header hdr;
        if (qti_probe(buffer,got,&hdr)!=0){ shell_error(sh,"qti: not a QTI icon"); return 1; }
        shell_printf(sh,"%s\n",resolved);
        shell_printf(sh,"  Format   QTI1 v%u\n",hdr.version);
        shell_printf(sh,"  Name     %.12s\n",hdr.name);
        shell_printf(sh,"  Frames   %u (16,32,64,128,256 – default 64)\n",hdr.frame_count);
        shell_printf(sh,"  Payload  %u bytes\n",hdr.payload_size);
        shell_printf(sh,"  Checksum 0x%08x\n",hdr.checksum);
        struct qti_image img;
        if (qti_load(resolved,256,&img)==0){
            shell_printf(sh,"  Largest  %dx%d\n",img.width,img.height);
            qti_free(&img);
        }
        return 0;
    }
    shell_error(sh,"qti: unknown subcommand"); return 1;
}

/* --- qtpkg ------------------------------------------------------------- */

static int cmd_qtpkg(struct shell *sh, int argc, char **argv)
{
    extern int qtpkg_command(struct shell *sh, int argc, char **argv);
    return qtpkg_command(sh, argc, argv);
}

/* --- fonts ------------------------------------------------------------- */

static int cmd_fonts(struct shell *sh, int argc, char **argv)
{
    if (argc<4 && argc>=2 && strcmp(argv[1],"set")==0){ shell_error(sh,"usage: fonts set <ui|terminal> <font-id>"); return 1; }
    if (argc>=4 && strcmp(argv[1],"set")==0){
        if (strcmp(argv[2],"ui")==0) font_set_ui(argv[3]);
        else if (strcmp(argv[2],"terminal")==0) font_set_terminal(argv[3]);
        else { shell_error(sh,"expected ui or terminal"); return 1; }
        shell_printf(sh,"%s font set to %s\n",argv[2],argv[3]); return 0;
    }
    shell_printf(sh,"%-18s %-10s %-6s %s\n","ID","WEIGHT","MONO","DESCRIPTION");
    for (int i=0;i<font_count();i++){
        const struct font *f=font_at(i);
        bool_t is_ui=(f==font_ui());
        bool_t is_term=(f==font_terminal());
        shell_printf(sh,"%-18s %-10s %-6s %s",f->id,f->weight==FONT_BOLD?"bold":"regular",f->monospace?"yes":"no",f->description);
        if (is_ui||is_term){
            shell_color(sh,"\033[92m");
            shell_printf(sh,"  [%s%s%s]",is_ui?"interface":"",(is_ui&&is_term)?", ":"",is_term?"terminal":"");
            shell_reset_color(sh);
        }
        shell_printf(sh,"\n");
    }
    shell_printf(sh,"\nChange: fonts set <ui|terminal> <id>\n");
    shell_printf(sh,"Specimen: The quick brown fox jumps over 1,234 lazy dogs. Il1O0\n");
    return 0;
}

static const struct shell_command commands[] = {
    {"fetch", "download URL over HTTP", "fetch <url> [-o file] [-H]", "Plain HTTP only – TLS not yet supported, https:// gives clear error.", cmd_fetch,0},
    {"lookup", "resolve hostname", "lookup <hostname>", NULL, cmd_lookup,0},
    {"qtx", "inspect/run Qito executables (.qtx)", "qtx <info|verify|exports|run|list> [file]", "QTX is native: QX header, 88B, format 'X'. See docs/QTX.md", cmd_qtx,0},
    {"qdl", "dynamic library manager (.qdl)", "qdl <list|load|unload|info> [file]", "QDL: format 'D', library flag, export table, refcounted, on-demand from /lib/*.qdl", cmd_qdl,0},
    {"qti", "inspect Qito icons (.qti)", "qti [list|info <file>]", "QTI: real pixels, 5 sizes 16/32/64/128/256 default 64, RAW/RLE/INDEX. See docs/QTI.md", cmd_qti,0},
    {"qtpkg", "package manager – install qasm/qcc from registry", "qtpkg <install|list|info> – qasm/qcc must be downloaded from https://github.com/qitoteam/qtpkg-registry, not bundled", "Entry /user/qtpkg/entry.var -> profile -> payload, TLS honest error for https://. qasm/qcc not bundled, must be installed via qtpkg from registry. See docs/QTPKG.md", cmd_qtpkg,0},
    {"fonts", "list/change typefaces", "fonts [set <ui|terminal> <id>]", NULL, cmd_fonts,0},
    {"lqx", "legacy alias for qtx", "qtx <...>", NULL, cmd_qtx, CMD_HIDDEN},
    {"qac", "legacy alias for qti", "qti <...>", NULL, cmd_qti, CMD_HIDDEN},
};

const struct shell_command *ultrashell_net_commands(int *count)
{
    *count = (int)ARRAY_SIZE(commands);
    return commands;
}
