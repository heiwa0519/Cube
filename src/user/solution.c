/*------------------------------------------------------------------------------
* solution.c : solution functions
*
*          Copyright (C) 2007-2018 by T.TAKASU, All rights reserved.
*
* reference :
*     [1] National Marine Electronic Association and International Marine
*         Electronics Association, NMEA 0183 version 4.10, August 1, 2012
*
* version : $Revision: 1.1 $ $Date: 2008/07/17 21:48:06 $
* history : 2007/11/03  1.0 new
*           2009/01/05  1.1  add function outsols(), outsolheads(),
*                            setsolformat(), outsolexs, outsolex
*           2009/04/02  1.2  add dummy fields in NMEA mesassage
*                            fix bug to format lat/lon as deg-min-sec
*           2009/04/14  1.3  add age and ratio field to solution
*           2009/11/25  1.4  add function readsolstat()
*           2010/02/14  1.5  fix bug on output of gpstime at week boundary
*           2010/07/05  1.6  added api:
*                                initsolbuf(),freesolbuf(),addsol(),getsol(),
*                                inputsol(),outprcopts(),outprcopt()
*                            modified api:
*                                readsol(),readsolt(),readsolstat(),
*                                readsolstatt(),outsolheads(),outsols(),
*                                outsolexs(),outsolhead(),outsol(),outsolex(),
*                                outnmea_rmc(),outnmea_gga(),outnmea_gsa(),
*                                outnmea_gsv()
*                            deleted api:
*                                setsolopt(),setsolformat()
*           2010/08/14  1.7  fix bug on initialize solution buffer (2.4.0_p2)
*                            suppress enu-solution if base pos not available
*                            (2.4.0_p3)
*           2010/08/16  1.8  suppress null record if solution is not avalilable
*                            (2.4.0_p4)
*           2011/01/23  1.9  fix bug on reading nmea solution data
*                            add api freesolstatbuf()
*           2012/02/05  1.10 fix bug on output nmea gpgsv
*           2013/02/18  1.11 support nmea GLGSA,GAGSA,GLCSV,GACSV sentence
*           2013/09/01  1.12 fix bug on presentation of nmea time tag
*           2015/02/11  1.13 fix bug on checksum of $GLGSA and $GAGSA
*                            fix bug on satellite id of $GAGSA
*           2016/01/17  1.14 support reading NMEA GxZDA
*                            ignore NMEA talker ID
*           2016/07/30  1.15 suppress output if std is over opt->maxsolstd
*           2017/06/13  1.16 support output/input of velocity solution
*           2018/10/10  1.17 support reading solution status file
*-----------------------------------------------------------------------------*/
#include <ctype.h>
#include "user.h"

/* constants and macros ------------------------------------------------------*/

#define SQR(x)     ((x)<0.0?-(x)*(x):(x)*(x))
#define SQRT(x)    ((x)<0.0||(x)!=(x)?0.0:sqrt(x))

#define MAXFIELD   64           /* max number of fields in a record */
#define MAXNMEA    256          /* max length of nmea sentence */

#define KNOT2M     0.514444444  /* m/knot */


/* solution option to field separator ----------------------------------------*/
static const char *opt2sep(const solopt_t *opt)
{
    if (!*opt->sep) return " ";
    else if (!strcmp(opt->sep,"\\t")) return "\t";
    return opt->sep;
}
/* separate fields -----------------------------------------------------------*/
static int tonum(char *buff, const char *sep, double *v)
{
    int n,len=(int)strlen(sep);
    char *p,*q;
    
    for (p=buff,n=0;n<MAXFIELD;p=q+len) {
        if ((q=strstr(p,sep))) *q='\0'; 
        if (*p) v[n++]=atof(p);
        if (!q) break;
    }
    return n;
}
/* sqrt of covariance --------------------------------------------------------*/
static double sqvar(double covar)
{
    return covar<0.0?-sqrt(-covar):sqrt(covar);
}
/* convert ddmm.mm in nmea format to deg -------------------------------------*/
static double dmm2deg(double dmm)
{
    return floor(dmm/100.0)+fmod(dmm,100.0)/60.0;
}
/* solution to covariance ----------------------------------------------------*/
static void soltocov(const sol_t *sol, double *P)
{
    P[0]     =sol->qr[0]; /* xx or ee */
    P[4]     =sol->qr[1]; /* yy or nn */
    P[8]     =sol->qr[2]; /* zz or uu */
    P[1]=P[3]=sol->qr[3]; /* xy or en */
    P[5]=P[7]=sol->qr[4]; /* yz or nu */
    P[2]=P[6]=sol->qr[5]; /* zx or ue */
}
/* solution to velocity covariance -------------------------------------------*/
static void soltocov_vel(const sol_t *sol, double *P)
{
    P[0]     =sol->qv[0]; /* xx */
    P[4]     =sol->qv[1]; /* yy */
    P[8]     =sol->qv[2]; /* zz */
    P[1]=P[3]=sol->qv[3]; /* xy */
    P[5]=P[7]=sol->qv[4]; /* yz */
    P[2]=P[6]=sol->qv[5]; /* zx */
}
/* output solution as the form of x/y/z-ecef ---------------------------------*/
static int outecef(unsigned char *buff, const char *s, const sol_t *sol,
                   const solopt_t *opt)
{
    const char *sep=opt2sep(opt);
    char *p=(char *)buff;
    
    trace(3,"outecef:\n");
    
    p+=sprintf(p,"%s%s%14.4f%s%14.4f%s%14.4f%s%3d%s%3d%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%6.2f%s%6.1f",
               s,sep,sol->rr[0],sep,sol->rr[1],sep,sol->rr[2],sep,sol->stat,sep,
               sol->ns,sep,SQRT(sol->qr[0]),sep,SQRT(sol->qr[1]),sep,SQRT(sol->qr[2]),
               sep,sqvar(sol->qr[3]),sep,sqvar(sol->qr[4]),sep,sqvar(sol->qr[5]),
               sep,sol->age,sep,sol->ratio);
    
    if (opt->outvel) { /* output velocity */
        p+=sprintf(p,"%s%10.5f%s%10.5f%s%10.5f%s%9.5f%s%8.5f%s%8.5f%s%8.5f%s%8.5f%s%8.5f",
                   sep,sol->rr[3],sep,sol->rr[4],sep,sol->rr[5],sep,
                   SQRT(sol->qv[0]),sep,SQRT(sol->qv[1]),sep,SQRT(sol->qv[2]),
                   sep,sqvar(sol->qv[3]),sep,sqvar(sol->qv[4]),sep,
                   sqvar(sol->qv[5]));
    }
    p+=sprintf(p,"\n");
    return (int)(p-(char *)buff);
}
/* output solution as the form of lat/lon/height -----------------------------*/
static int outpos(unsigned char *buff, const char *s, const sol_t *sol,
                  const solopt_t *opt)
{
    double pos[3],vel[3],dms1[3],dms2[3],P[9],Q[9];
    const char *sep=opt2sep(opt);
    char *p=(char *)buff;
    
    trace(3,"outpos  :\n");
    
    ecef2pos(sol->rr,pos);
    soltocov(sol,P);
    covenu(pos,P,Q);

    if (opt->degf) {
        deg2dms(pos[0]*R2D,dms1,5);
        deg2dms(pos[1]*R2D,dms2,5);
        p+=sprintf(p,"%s%s%4.0f%s%02.0f%s%08.5f%s%4.0f%s%02.0f%s%08.5f",s,sep,
                   dms1[0],sep,dms1[1],sep,dms1[2],sep,dms2[0],sep,dms2[1],sep,
                   dms2[2]);
    }
    else {
        p+=sprintf(p,"%s%s%14.9f%s%14.9f",s,sep,pos[0]*R2D,sep,pos[1]*R2D);
    }
    p+=sprintf(p,"%s%10.4f%s%3d%s%3d%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%6.2f%s%6.1f",
               sep,pos[2],sep,sol->stat,sep,sol->ns,sep,SQRT(Q[4]),sep,
               SQRT(Q[0]),sep,SQRT(Q[8]),sep,sqvar(Q[1]),sep,sqvar(Q[2]),
               sep,sqvar(Q[5]),sep,sol->age,sep,sol->ratio);
    
    if (opt->outvel) { /* output velocity */
        soltocov_vel(sol,P);
        ecef2enu(pos,sol->rr+3,vel);
        covenu(pos,P,Q);
        p+=sprintf(p,"%s%10.5f%s%10.5f%s%10.5f%s%9.5f%s%8.5f%s%8.5f%s%8.5f%s%8.5f%s%8.5f",
                   sep,vel[1],sep,vel[0],sep,vel[2],sep,SQRT(Q[4]),sep,
                   SQRT(Q[0]),sep,SQRT(Q[8]),sep,sqvar(Q[1]),sep,sqvar(Q[2]),
                   sep,sqvar(Q[5]));
    }
    p+=sprintf(p,"\n");
    return (int)(p-(char *)buff);
}
/* output solution as the form of e/n/u-baseline -----------------------------*/
static int outenu(unsigned char *buff, const char *s, const sol_t *sol,
                  const double *rb, const solopt_t *opt)
{
    double pos[3],rr[3],enu[3],P[9],Q[9];
    int i;
    const char *sep=opt2sep(opt);
    char *p=(char *)buff;
    
    trace(3,"outenu  :\n");
    
    for (i=0;i<3;i++) rr[i]=sol->rr[i]-rb[i];
    ecef2pos(rb,pos);
    soltocov(sol,P);
    covenu(pos,P,Q);
    ecef2enu(pos,rr,enu);
    p+=sprintf(p,"%s%s%14.4f%s%14.4f%s%14.4f%s%3d%s%3d%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%8.4f%s%6.2f%s%6.1f\n",
               s,sep,enu[0],sep,enu[1],sep,enu[2],sep,sol->stat,sep,sol->ns,sep,
               SQRT(Q[0]),sep,SQRT(Q[4]),sep,SQRT(Q[8]),sep,sqvar(Q[1]),
               sep,sqvar(Q[5]),sep,sqvar(Q[2]),sep,sol->age,sep,sol->ratio);
    return (int)(p-(char *)buff);
}
/* output processing options ---------------------------------------------------
* output processing options to buffer
* args   : unsigned char *buff IO output buffer
*          prcopt_t *opt    I   processign options
* return : number of output bytes
*-----------------------------------------------------------------------------*/
extern int outprcopts(unsigned char *buff, const prcopt_t *opt)
{
    const int sys[]={SYS_GPS,SYS_GLO,SYS_GAL,SYS_QZS,SYS_CMP,SYS_IRN,SYS_SBS,0};
    const char *s1[]={"single","dgps","kinematic","static","moving-base","fixed",
                 "ppp-kinematic","ppp-static","ppp-fixed",""};
    const char *s2[]={"L1","L1+L2","L1+L2+L5","L1+L2+L5+L6","L1+L2+L5+L6+L7",
                      "L1+L2+L5+L6+L7+L8",""};
    const char *s3[]={"forward","backward","combined"};
    const char *s4[]={"off","broadcast","sbas","iono-free","estimation",
                      "ionex tec","qzs","lex","vtec_sf","vtec_ef","gtec",""};
    const char *s5[]={"off","saastamoinen","sbas","est ztd","est ztd+grad",""};
    const char *s6[]={"broadcast","precise","broadcast+sbas","broadcast+ssr apc",
                      "broadcast+ssr com","qzss lex",""};
    const char *s7[]={"gps","glonass","galileo","qzss","beidou","irnss","sbas",""};
    const char *s8[]={"off","continuous","instantaneous","fix and hold",""};
    const char *s9[]={"off","on","auto calib","external calib",""};
    int i;
    char *p=(char *)buff;
    
    trace(3,"outprcopts:\n");
    
    p+=sprintf(p,"%s pos mode  : %s\n",COMMENTH,s1[opt->mode]);
    
    //if (PMODE_DGPS<=opt->mode&&opt->mode<=PMODE_FIXED) {
    //    p+=sprintf(p,"%s freqs     : %s\n",COMMENTH,s2[opt->nf-1]);
    //}
    if (opt->mode>PMODE_SINGLE) {
        p+=sprintf(p,"%s solution  : %s\n",COMMENTH,s3[opt->soltype]);
    }
    p+=sprintf(p,"%s elev mask : %.1f deg\n",COMMENTH,opt->elmin*R2D);
    //if (opt->mode>PMODE_SINGLE) {
    //    p+=sprintf(p,"%s dynamics  : %s\n",COMMENTH,opt->dynamics?"on":"off");
    //    p+=sprintf(p,"%s tidecorr  : %s\n",COMMENTH,opt->tidecorr?"on":"off");
    //}
    if (opt->mode<=PMODE_FIXED) {
        p+=sprintf(p,"%s ionos opt : %s\n",COMMENTH,s4[opt->ionoopt]);
    }
    p+=sprintf(p,"%s tropo opt : %s\n",COMMENTH,s5[opt->tropopt]);
    p+=sprintf(p,"%s ephemeris : %s\n",COMMENTH,s6[opt->sateph]);
    if (opt->navsys!=SYS_GPS) {
        p+=sprintf(p,"%s navi sys  :",COMMENTH);
        for (i=0;sys[i];i++) {
            if (opt->navsys&sys[i]) p+=sprintf(p," %s",s7[i]);
        }
        p+=sprintf(p,"\n");
    }
    if (PMODE_KINEMA<=opt->mode&&opt->mode<=PMODE_FIXED) {
        p+=sprintf(p,"%s amb res   : %s\n",COMMENTH,s8[opt->modear]);
        //if (opt->navsys&SYS_GLO) {
        //    p+=sprintf(p,"%s amb glo   : %s\n",COMMENTH,s9[opt->glomodear]);
        //}
        //if (opt->thresar[0]>0.0) {
        //    p+=sprintf(p,"%s val thres : %.1f\n",COMMENTH,opt->thresar[0]);
        //}
    }
    //if (opt->mode==PMODE_MOVEB&&opt->baseline[0]>0.0) {
    //    p+=sprintf(p,"%s baseline  : %.4f %.4f m\n",COMMENTH,
    //               opt->baseline[0],opt->baseline[1]);
    //}
    for (i=0;i<2;i++) {
        if (opt->mode==PMODE_SINGLE||(i>=1&&opt->mode>PMODE_FIXED)) continue;
        p+=sprintf(p,"%s antenna%d  : %-21s (%7.4f %7.4f %7.4f)\n",COMMENTH,
                   i+1,opt->anttype[i],opt->antdel[i][0],opt->antdel[i][1],
                   opt->antdel[i][2]);
    }
    return (int)(p-(char *)buff);
}
/* output solution header ------------------------------------------------------
* output solution header to buffer
* args   : unsigned char *buff IO output buffer
*          solopt_t *opt    I   solution options
* return : number of output bytes
*-----------------------------------------------------------------------------*/
extern int outsolheads(unsigned char *buff, const solopt_t *opt)
{
    const char *s1[]={"WGS84","Tokyo"},*s2[]={"ellipsoidal","geodetic"};
    const char *s3[]={"GPST","UTC ","JST "},*sep=opt2sep(opt);
    char *p=(char *)buff;
    int timeu=opt->timeu<0?0:(opt->timeu>20?20:opt->timeu);
    
    trace(3,"outsolheads:\n");
    
    if (opt->posf==SOLF_NMEA||opt->posf==SOLF_STAT||opt->posf==SOLF_GSIF) {
        return 0;
    }
    if (opt->outhead) {
        p+=sprintf(p,"%s (",COMMENTH);
        if      (opt->posf==SOLF_XYZ) p+=sprintf(p,"x/y/z-ecef=WGS84");
        else if (opt->posf==SOLF_ENU) p+=sprintf(p,"e/n/u-baseline=WGS84");
        else p+=sprintf(p,"lat/lon/height=%s/%s",s1[opt->datum],s2[opt->height]);
        p+=sprintf(p,",Q=1:fix,2:float,3:sbas,4:dgps,5:single,6:ppp,ns=# of satellites)\n");
    }
    p+=sprintf(p,"%s  %-*s%s",COMMENTH,(opt->timef?16:8)+timeu+1,s3[opt->times],sep);
    
    if (opt->posf==SOLF_LLH) { /* lat/lon/hgt */
        if (opt->degf) {
            p+=sprintf(p,"%16s%s%16s%s%10s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s",
                       "latitude(d'\")",sep,"longitude(d'\")",sep,"height(m)",sep,
                       "Q",sep,"ns",sep,"sdn(m)",sep,"sde(m)",sep,"sdu(m)",sep,
                       "sdne(m)",sep,"sdeu(m)",sep,"sdue(m)",sep,"age(s)",sep,"ratio");
        }
        else {
            p+=sprintf(p,"%14s%s%14s%s%10s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s",
                       "latitude(deg)",sep,"longitude(deg)",sep,"height(m)",sep,
                       "Q",sep,"ns",sep,"sdn(m)",sep,"sde(m)",sep,"sdu(m)",sep,
                       "sdne(m)",sep,"sdeu(m)",sep,"sdun(m)",sep,"age(s)",sep,"ratio");
        }
        if (opt->outvel) {
            p+=sprintf(p,"%s%10s%s%10s%s%10s%s%9s%s%8s%s%8s%s%8s%s%8s%s%8s",
                       sep,"vn(m/s)",sep,"ve(m/s)",sep,"vu(m/s)",sep,"sdvn",sep,
                       "sdve",sep,"sdvu",sep,"sdvne",sep,"sdveu",sep,"sdvun");
        }
    }
    else if (opt->posf==SOLF_XYZ) { /* x/y/z-ecef */
        p+=sprintf(p,"%14s%s%14s%s%14s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s",
                   "x-ecef(m)",sep,"y-ecef(m)",sep,"z-ecef(m)",sep,"Q",sep,"ns",sep,
                   "sdx(m)",sep,"sdy(m)",sep,"sdz(m)",sep,"sdxy(m)",sep,
                   "sdyz(m)",sep,"sdzx(m)",sep,"age(s)",sep,"ratio");
        
        if (opt->outvel) {
            p+=sprintf(p,"%s%10s%s%10s%s%10s%s%9s%s%8s%s%8s%s%8s%s%8s%s%8s",
                       sep,"vx(m/s)",sep,"vy(m/s)",sep,"vz(m/s)",sep,"sdvx",sep,
                       "sdvy",sep,"sdvz",sep,"sdvxy",sep,"sdvyz",sep,"sdvzx");
        }
    }
    else if (opt->posf==SOLF_ENU) { /* e/n/u-baseline */
        p+=sprintf(p,"%14s%s%14s%s%14s%s%3s%s%3s%s%8s%s%8s%s%8s%s%8s%s%8s%s%8s%s%6s%s%6s",
                   "e-baseline(m)",sep,"n-baseline(m)",sep,"u-baseline(m)",sep,
                   "Q",sep,"ns",sep,"sde(m)",sep,"sdn(m)",sep,"sdu(m)",sep,
                   "sden(m)",sep,"sdnu(m)",sep,"sdue(m)",sep,"age(s)",sep,"ratio");
    }
    p+=sprintf(p,"\n");
    return (int)(p-(char *)buff);
}
/* std-dev of soltuion -------------------------------------------------------*/
static double sol_std(const sol_t *sol)
{
    /* approximate as max std-dev of 3-axis std-devs */
    if (sol->qr[0]>sol->qr[1]&&sol->qr[0]>sol->qr[2]) return SQRT(sol->qr[0]);
    if (sol->qr[1]>sol->qr[2]) return SQRT(sol->qr[1]);
    return SQRT(sol->qr[2]);
}
/* output solution body --------------------------------------------------------
* output solution body to buffer
* args   : unsigned char *buff IO output buffer
*          sol_t  *sol      I   solution
*          double *rb       I   base station position {x,y,z} (ecef) (m)
*          solopt_t *opt    I   solution options
* return : number of output bytes
*-----------------------------------------------------------------------------*/
extern int outsols(unsigned char *buff, const sol_t *sol, const double *rb,
                   const solopt_t *opt)
{
    gtime_t time,ts={0};
    double gpst;
    int week,timeu;
    const char *sep=opt2sep(opt);
    char s[64];
    unsigned char *p=buff;
    
    trace(3,"outsols :\n");
    
    /* suppress output if std is over opt->maxsolstd */
    if (opt->maxsolstd>0.0&&sol_std(sol)>opt->maxsolstd) {
        return 0;
    }

    if (sol->stat<=SOLQ_NONE||(opt->posf==SOLF_ENU&&norm(rb,3)<=0.0)) {
        return 0;
    }
    timeu=opt->timeu<0?0:(opt->timeu>20?20:opt->timeu);
    
    time=sol->time;
    if (opt->times>=TIMES_UTC) time=gpst2utc(time);
    if (opt->times==TIMES_JST) time=timeadd(time,9*3600.0);
    
    if (opt->timef) time2str(time,s,timeu);
    else {
        gpst=time2gpst(time,&week);
        if (86400*7-gpst<0.5/pow(10.0,timeu)) {
            week++;
            gpst=0.0;
        }
        sprintf(s,"%4d%s%*.*f",week,sep,6+(timeu<=0?0:timeu+1),timeu,gpst);
    }
    switch (opt->posf) {
        case SOLF_LLH:  p+=outpos (p,s,sol,opt);   break;
        case SOLF_XYZ:  p+=outecef(p,s,sol,opt);   break;
        case SOLF_ENU:  p+=outenu(p,s,sol,rb,opt); break;
    }
    return (int)(p-buff);
}
/* output processing option ----------------------------------------------------
* output processing option to file
* args   : FILE   *fp       I   output file pointer
*          prcopt_t *opt    I   processing options
* return : none
*-----------------------------------------------------------------------------*/
extern void outprcopt(FILE *fp, const prcopt_t *opt)
{
    unsigned char buff[MAXSOLMSG+1];
    int n;
    
    trace(3,"outprcopt:\n");
    
    if ((n=outprcopts(buff,opt))>0) {
        fwrite(buff,n,1,fp);
    }
}
/* output solution header ------------------------------------------------------
* output solution heade to file
* args   : FILE   *fp       I   output file pointer
*          solopt_t *opt    I   solution options
* return : none
*-----------------------------------------------------------------------------*/
extern void outsolhead(FILE *fp, const solopt_t *opt)
{
    unsigned char buff[MAXSOLMSG+1];
    int n;
    
    trace(3,"outsolhead:\n");
    
    if ((n=outsolheads(buff,opt))>0) {
        fwrite(buff,n,1,fp);
    }
}
/* output solution body --------------------------------------------------------
* output solution body to file
* args   : FILE   *fp       I   output file pointer
*          sol_t  *sol      I   solution
*          double *rb       I   base station position {x,y,z} (ecef) (m)
*          solopt_t *opt    I   solution options
* return : none
*-----------------------------------------------------------------------------*/
extern void outsol(FILE *fp, const sol_t *sol, const double *rb,
                   const solopt_t *opt)
{
    unsigned char buff[MAXSOLMSG+1];
    int n;
    
    trace(3,"outsol  :\n");
    
    if ((n=outsols(buff,sol,rb,opt))>0) {
        fwrite(buff,n,1,fp);
    }
}