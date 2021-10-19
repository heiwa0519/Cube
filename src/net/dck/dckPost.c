
#include "dck.h"

#define OUT_OBS_TEST	0
#define OUT_OBS_TYPE	0

/* execute processing network ------------------------------------------------*/
static int exeNetSession_d(net_t *net, char *outfile, int nsta)
{
	FILE *fp;
	gtime_t time,ts,endTime;
	nav_t *nav=getnav_t();
	rcv_t *rcv=getrcv_t();
	int i,stat=0;

	trace(3,"exeNetSession_d:\n");

	if (!(fp=fopen(outfile,"w"))) {
		trace(2,"open outfile error file=%s\n",outfile);
		return 0;
	}

#if OUT_OBS_TEST
	FILE *fpo=fopen("obs.txt","w");
#endif

#if OUT_OBS_TYPE
	FILE *fpt=fopen("obs_type.txt","w");
#endif

	ts=net->opt.time;
	while (getMultiOBSBody(ts,"",rcv,nsta)) {
		showmsg("%s Q=%d",time_str(ts,2),net->sol.stat);

		if (net->opt.ts.time!=0) {
			if (timediff(ts,net->opt.ts)<0.0) {
				ts.time+=30;
				continue;
			}
		}
		if (net->opt.te.time!=0) {
			if (timediff(ts,net->opt.te)>0.0) break;
		}

		ts.time+=30;

#if OUT_OBS_TEST
		outObsData(fpo,rcv,nsta);
		continue;
#endif

#if OUT_OBS_TYPE
		outObsType(fpt,rcv,nsta);
		break;
#endif

		excludeObs(rcv,&net->opt,nsta);

		net->nobs=0;
		for (i=0;i<nsta&&i<MAXRCV;i++) {
			/* invalid station */
			if(rcv[i].nobs==0) continue;
			net->nobs+=rcv[i].nobs;

			time=rcv[i].time;
			rcv[i].time=rcv[i].obs[0].time;
			if (time.time!=0) rcv[i].tt=timediff(rcv[i].time,time);
			net->sol.time=rcv[i].time;
		}
		time2str(net->sol.time,net->time,2);

		/* first epoch and with constraint */
		if (net->eflag==FST_EPOCH&&net->opt.inh) {
			if (copyConstraint_d(net,rcv,nsta)) {
				net->eflag|=MID_EPOCH;
				for (i=0;i<nsta&&i<MAXRCV;i++) rcv[i].tt=30.0;
			}
			else {
				trace(2, "execSession: invalid constraint\n");
				fclose(fp);
				return 0;
			}
		}

		/* if last epoch */
		endTime=timeadd(net->sol.time,30.0);
		if (endTime.time%86400==0) net->eflag=LST_EPOCH;

		if (!estProduct_d(net,rcv,nsta,nav,net->eflag)) {stat=1;break;}
		/*if (flag) */outProduct_d(fp,net,rcv,nsta);
		logRes_d(net,rcv,nsta);
		logZtd_d(net,rcv,nsta);
		logAmb_d(net,rcv,nsta);
		logDtm(net->sol.time,rcv,nsta);
		net->eflag|=MID_EPOCH;
	}

#if OUT_OBS_TEST
	fclose(fpo);
#endif

#if OUT_OBS_TYPE
	fclose(fpt);
#endif

	fclose(fp);
	return stat?0:1;
}
/* open procssing session ----------------------------------------------------*/
static int openNetSession_d(net_t *dck, const filopt_t *fopt, const char *obsfile,
	const char *outfile)
{
	gtime_t ptime[3];
	pcvs_t *pcv=getpcvs_t();
	nav_t  *nav=getnav_t();
	rcv_t  *rcv=getrcv_t();
	char file[1024],file2[1024];
	int i,n,sat[MAXSAT]={0};

	/* time of day before and day after */
	ptime[1]=dck->opt.time;
	ptime[0]=timeadd(ptime[1],-86400.0);
	ptime[2]=timeadd(ptime[1], 86400.0);

	/* read ephemeris files, today and previous day */
	reppath(fopt->nav,file,ptime[1]); fprintf(stdout,"read: %s\n",file);
	if (!readrnx(file,0,"",NULL,nav,NULL)) return 0;

	/* delete duplicated ephemeris */
	uniqnav(nav);

	/* read precise ephemeris files in 3 days */
	for (i=0;i<3;i++) {
		reppath(fopt->sp3,file,ptime[i]); fprintf(stdout,"read: %s\n",file);
		readsp3(file,nav,0);
	}
	if (nav->ne<=0) {
		trace(2,"no sp3 data\n");
		return 0;
	}

	/* read precise satellite clocks */
	if (!dck->opt.estclk) for (i=0;i<3;i++) {
		reppath(fopt->clk,file,ptime[i]); fprintf(stdout,"read: %s\n",file);
		readrnxc(file,nav);
	}
	if (!dck->opt.estclk&&nav->nc<=0) {
		trace(2,"no clk data\n");
		return 0;
	}

	/* read receiver and satellite antenna parameters */
	fprintf(stdout,"read: %s\n",fopt->atx);
	if (!readpcv(fopt->atx,pcv)) {
		trace(2,"read antenna pcv error: %s\n",fopt->atx);
		return 0;
	}
	/* use satellite L2 offset if L5 offset does not exists */
	for (i=0;i<pcv->n;i++) {
		if (norm(pcv->pcv[i].off[2],3)>0.0) continue;
		matcpy(pcv->pcv[i].off[2],pcv->pcv[i].off[1],3,1);
		matcpy(pcv->pcv[i].var[2],pcv->pcv[i].var[1],19,1);
	}

	/* read erp data */
	reppath(fopt->erp,file,ptime[1]);
	fprintf(stdout,"read: %s\n",file);
	if (!readerp(file,&nav->erp)) { 
		trace(2,"no erp data %s\n",file);
		return 0;
	}

	/* read dcb parameters */
	reppath(fopt->dcb,file,ptime[1]);
	fprintf(stdout,"read: %s\n",file);
	if (!readdcb(file,nav,NULL)) {
		trace(2,"no dcb data %s\n",file); return 0;
	}

	/* open multi-receiver observation file pointers */
	if ((n=openMultiOBSFile(obsfile))==0) {
		trace(1,"no multi-receiver observation files: %s\n",obsfile);
		return 0;
	}
	getMultiOBSHeader(rcv,n);
	dck->opt.ircv=1;

	/* read station sinex coordinates */
	reppath(fopt->snx,file,ptime[1]);
	fprintf(stdout,"read: %s\n",file);
	if (!readSnxNet(file,rcv,n)) return 0;

	/* read ocean tide loading parameters */
	fprintf(stdout,"read: %s\n",fopt->blq);
	readOtlNet(fopt->blq,rcv,n);

	/* read record for constraint */
	if (dck->opt.inh) {
		reppath(fopt->ind,file ,ptime[0]);fprintf(stdout,"read: %s\n",file);
		reppath(fopt->var,file2,ptime[0]);fprintf(stdout,"read: %s\n",file2);
		if (!getRecord_d(dck,file,file2)) return 0;
	}

	/* output rcv positions to datum file */
	logDatumRcvPos(rcv,n);

	/* set pcv for satellite and receivers */
	setPcvNet(ptime[1],&dck->opt,nav,pcv,rcv,n);


	return n;
}
/* close procssing session ---------------------------------------------------*/
static void closeNetSession_d()
{
	pcvs_t *pcv=getpcvs_t();
	obs_t *obs=getobs_t();
	nav_t *nav=getnav_t();

	trace(3,"closeses:\n");

	free(nav->eph);		nav->eph	 =NULL;nav->n =nav->nmax =0;
	free(nav->geph);	nav->geph	 =NULL;nav->ng=nav->ngmax=0;
	free(nav->seph);	nav->seph	 =NULL;nav->ns=nav->nsmax=0;
	free(nav->peph);	nav->peph	 =NULL;nav->ne=nav->nemax=0;
	free(nav->pclk);	nav->pclk	 =NULL;nav->nc=nav->ncmax=0;
	free(nav->tec);		nav->tec	 =NULL;nav->nt=nav->ntmax=0;
	free(nav->erp.data);nav->erp.data=NULL;nav->erp.n=nav->erp.nmax=0;
	free(pcv->pcv);		pcv->pcv	 =NULL;pcv->n =pcv->nmax =0;
	free(obs->data);	obs->data	 =NULL;obs->n =obs->nmax =0;

	/* close log files */
	traceclose();  closeDtmLog(); closeResLog_d();
	closeZtdLog_d(); closeAmbLog_d();
}
/* open log files ------------------------------------------------------------*/
static void openLogFile(const prcopt_t *opt, const solopt_t *sopt,
	const char *outfile)
{
	char outf[1024];

	/* open residuals */
	strcpy(outf,outfile); strcat(outf,".res");
	closeResLog_d(); openResLog_d(outf); fprintf(stdout,"out: %s\n",outf);

	/* open ztd */
	strcpy(outf,outfile); strcat(outf,".ztd");
	closeZtdLog_d(); openZtdLog_d(outf,opt->tropopt);
	fprintf(stdout,"out: %s\n",outf);

	/* open ambiguity */
	strcpy(outf,outfile); strcat(outf,".amb");
	closeAmbLog_d(); openAmbLog_d(outf); fprintf(stdout,"out: %s\n",outf);

	/* open ambiguity datum */
	strcpy(outf,outfile); strcat(outf,".dtm");
	closeDtmLog(); openDtmLog(outf); fprintf(stdout,"out: %s\n",outf);

	/* open debug trace */
	if (sopt->trace>0) {
		strcpy(outf,outfile); strcat(outf,".trace");
		traceclose(); traceopen(outf); fprintf(stdout,"out: %s\n",outf);
		tracelevel(sopt->trace);
	}

	/* open record files */
	recordOutFile_d(outfile);
}
/* free memory for net control struct ----------------------------------------*/
static void freeNet(net_t *net)
{
	net->nx=0;
	free(net->x);net->x=NULL;
	free(net->P);net->P=NULL;
	//free(net->xa);net->xa=NULL;
	//free(net->Pa);net->Pa=NULL;

	net->nc=net->ncmax=0;
	net->nd=net->ndmax=0;
	free(net->csd);net->csd=NULL;
	free(net->dtm);net->dtm=NULL;
	free(net->DP); net->DP =NULL; 
	//net->skipfix=0;
}
/* initialize net control ----------------------------------------------------*/
static void initNet(net_t *net, const prcopt_t *opt)
{
	netsol_t sol0={{0}};
	int i;

	net->nx=MAXRCV*DNR(opt)+DNSC(opt);
	net->x =zeros(net->nx,1);net->P =zeros(net->nx,net->nx);
	//net->xa=zeros(net->nx,1);net->Pa=zeros(net->nx,net->nx);
	net->sol=sol0;
	net->opt=*opt;
	net->nobs=0;
	net->fixedrec=0;
	for (i=0;i<MAXSAT;i++) {net->SSC[i]=0; net->outc[i]=0;}
	for (i=0;i<MAXRCV;i++) net->SRC[i]=0;
	net->time[0]='\0';
	net->eflag=FST_EPOCH;

	net->ctime.time=0;net->ctime.sec=0.0;
	net->nc=net->ncmax=net->nd=net->ndmax=0;
	net->csd=NULL;
	net->dtm=NULL;
	net->DP=NULL;

	//net->skipfix=0;
}
#define ROUND(x)        (int)floor((x)+0.5)
/* estimate clock products by ppp network solution ---------------------------
* generate decoupled clock products
* args  : prcopt_t  *opt    I   processing options
*         solopt_t  *sopt   I   solution options
*         filopt_t  *fopt   I   file options
* return: status (0:error,1:ok) 
*		   inputs files can include wild-cards (*). if an file includes
*          wild-cards, the wild-card expanded multiple files are used.
* ---------------------------------------------------------------------------*/
extern int dckPost(const prcopt_t *popt, const solopt_t *sopt, 
	const filopt_t *fopt)
{
	prcopt_t opt=*popt;
	net_t net;
	gtime_t time;
	double ep[6];
	char obsfile[1024],outfile[1024],solution[1024],temp[1024],name[32],*p,*q;
	int week,dow,stat=0,i,n,pre_n;

	trace(3,"dckPost:\n");

	/* name solution folder */
	p=solution;
	sprintf(p,"%s","net_solution");
	p+=strlen(solution);
	sprintf(p,"%s","_dc"); p+=3;

	/* attention: only same rcvs between days */
	for (i=0;i<opt.netdays;i++) {
		opt.time.time=popt->time.time+(i*86400);

		/* utc to current time zone */
		time=timeadd(timeget(),NET_TIMEZONE*3600.0);
		time2epoch(time,ep);

		q=name;
		sprintf(q,"%04.0f", ep[0]); q+=4;	/* year */
		sprintf(q,"%02.0f", ep[1]); q+=2;	/* month */
		sprintf(q,"%02.0f_",ep[2]); q+=3;	/* day */
		sprintf(q,"%02.0f", ep[3]); q+=2;	/* hour */
		sprintf(q,"%02.0f", ep[4]); q+=2;	/* hour */
		sprintf(p,"_%s",name);

		/* configure output path, apm21210.list for example */
		reppath(fopt->obs,outfile,opt.time);
		strcpy(obsfile,outfile);
		if (q=strrchr(outfile,FILEPATHSEP)) {
			dow=ROUND(time2gpst(opt.time,&week)/86400.0);
			sprintf(q+1,"%s%capm%04d%d.clk",solution,FILEPATHSEP,week,dow);
		}

		if ((q=strstr(obsfile,"_*.list"))&&opt.netdays==2) {
			if (i) {
				time=timeadd(opt.time,-86400.0);
				dow=ROUND(time2gpst(time,&week)/86400.0);
				sprintf(q+1,"%04d%d.list",week,dow);
			}
			else {
				time=timeadd(opt.time,86400.0);
				dow=ROUND(time2gpst(time,&week)/86400.0);
				sprintf(q+1,"%04d%d.list",week,dow);
			}
		}

		fprintf(stdout,"read: %s\n",obsfile);
		fprintf(stdout,"out: %s\n",outfile);

		/* open log files */
		openLogFile(&opt,sopt,outfile);

		if (!i) initNet(&net,&opt);
		else {
			net.opt=opt;
			net.eflag=MID_EPOCH;
		}
		if (!(n=openNetSession_d(&net,fopt,obsfile,outfile))) {
			closeNetSession_d();
			return 0;
		}
		if (i&&n!=pre_n) {
			trace(2,"not equal rcvs pre_n=%d now_n=%d",pre_n,n);
			closeNetSession_d();
			freeNet(&net);
			return 0;
		}
		sprintf(temp,"%s_temp",outfile);
		stat=exeNetSession_d(&net,temp,n);
		closeNetSession_d();
		if (!stat) break;

		generateFinalClk(&opt,getrcv_t(),n,temp,fopt->atx,outfile);
		pre_n=n;
		fprintf(stdout,"\n\n");
	}

	freeNet(&net);
	return stat;
}