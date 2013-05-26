#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <getopt.h>
#include <assert.h>

// QUBO solver
// Solves random instances of QUBO problem as described in section 3.2 of
// http://www.cs.amherst.edu/ccm/cf14-mcgeoch.pdf
// 
// Chimera graph, C_N:
// Vertices are (x,y,o,i)  0<=x,y<N, 0<=o<2, 0<=i<4
// Edge from (x,y,o,i) to (y',x',o',i') if
// (x,y)=(x',y'), o!=o', OR
// |x-x'|=1, y=y', o=o'=0, i=i', OR
// |y-y'|=1, x=x', o=o'=1, i=i'
// 
// x,y are the horizontal,vertical co-ords of the K4,4
// o is the "orientation" (0=horizontally connected, 1=vertically connected)
// i is the index within the "semi-K4,4"
// There is an involution given by {x<->y o<->1-o}

#define N 8
#define NV (8*N*N)
#define NE (8*N*(3*N-1))
#define NBV (2*N*N)
#define NBE (N*(3*N-2))
#define enc(x,y,o,i) ((i)+((o)<<2)+(((x)*N+(y))<<3))
#define decx(p) (((p)>>3)/N)
#define decy(p) (((p)>>3)%N)
#define deco(p) (((p)>>2)&1)
#define deci(p) ((p)&3)

int adj[NV][6];// Neighbours as encoded vertices (-1 for non-existent)
               // 0-3 corresponds to intra-K_4,4 neighbours
               // 4 = Left or Down, 5 = Right or Up
int Q[NV][6];// Weights
int elist[NE][4]; // elist[e][0] = encoded start vertex of edge e
                  // elist[e][1] = edge number (0..5) from vertex elist[e][0] to elist[e][2]
                  // elist[e][2] = encoded end vertex of edge e
                  // elist[e][3] = edge number (0..5) from vertex elist[e][2] to elist[e][0]
int XBplus[N+2][N][2];
int (*XB)[N][2]=&XBplus[1];// XB[x][y][o] = State (0..15) of big vert
                           // Allow extra space to avoid having to check for out-of-bounds accesses
int QB[N][N][2][3][16][16]; // Weights for big verts (derived from Q[])
                            // QB[x][y][o][d][s0][s1] = total weight from big vert (x,y,o) in state s0
                            //                          to the big vert in direction d in state s1
                            // d=0 is intra K_4,4, d=1 is Left/Down, d=2 is Right/Up
//#define X(p) (statemap[(XB[decx(p)][decy(p)][deco(p)]>>deci(p))&1])// ncu
int okv[NV];

int wn,seed,bm;
double maxt;
char *infile,*outfile;
int statemap[2];

#define MAX(x,y) ((x)>(y)?(x):(y))
// Isolate random number generator in case we need to replace it with something better
void initrand(int seed){srandom(seed);}
int randbit(void){return (random()>>16)&1;}
int randnib(void){return (random()>>16)&15;}
int randint(int n){return random()%n;}

double cpu(){return clock()/(double)CLOCKS_PER_SEC;}

void initgraph(void){
  int i,j,o,p,q,x,y,nv,ne;
  nv=ne=0;
  for(x=0;x<N;x++)for(y=0;y<N;y++)for(o=0;o<2;o++)for(i=0;i<4;i++){
    p=enc(x,y,o,i);nv++;
    for(j=0;j<4;j++)adj[p][j]=enc(x,y,1-o,j);
    if(o==0){
      adj[p][4]=x>0?enc(x-1,y,o,i):-1;
      adj[p][5]=x<N-1?enc(x+1,y,o,i):-1;
    }else{
      adj[p][4]=y>0?enc(x,y-1,o,i):-1;
      adj[p][5]=y<N-1?enc(x,y+1,o,i):-1;
    }
    for(j=0;j<6;j++){
      q=adj[p][j];
      if(p<q){
        elist[ne][0]=p;elist[ne][1]=j;
        elist[ne][2]=q;elist[ne][3]=(j<4?i:9-j);
        ne++;
      }
    }
  }
  assert(nv==NV&&ne==NE);
}

void getbigweights(void){// Get derived weights on "big graph" QB[] from Q[]
  int i,j,o,p,t,x,y,s0,s1;
  for(x=0;x<N;x++)for(y=0;y<N;y++)for(o=0;o<2;o++)for(s0=0;s0<16;s0++)for(s1=0;s1<16;s1++){
    p=enc(x,y,o,0);
    t=0;
    for(i=0;i<4;i++)for(j=0;j<4;j++)t+=Q[p+i][j]*statemap[(s0>>i)&1]*statemap[(s1>>j)&1];
    QB[x][y][o][0][s0][s1]=t;
    t=0;
    for(i=0;i<4;i++)t+=Q[p+i][4]*statemap[(s0>>i)&1]*statemap[(s1>>i)&1];
    QB[x][y][o][1][s0][s1]=t;
    t=0;
    for(i=0;i<4;i++)t+=Q[p+i][5]*statemap[(s0>>i)&1]*statemap[(s1>>i)&1];
    QB[x][y][o][2][s0][s1]=t;
  }
}

void initweights(void){// Initialise a symmetric weight matrix with random +/-1s,
  //                      and a random subset of wn working vertices
  int i,j,t,v0,v1;
  for(i=0;i<NV;i++)for(j=0;j<6;j++)Q[i][j]=0; // Ensure weight=0 for non-existent edges
  for(i=0,t=wn;i<NV;i++){okv[i]=(randint(NV-i)<t);t-=okv[i];}
  for(i=0;i<NE;i++){
    v0=elist[i][0];v1=elist[i][2];
    Q[v0][elist[i][1]]=Q[v1][elist[i][3]]=(randbit()*2-1)*okv[v0]*okv[v1];
  }
  getbigweights();
}

void init_state(void){// Initialise state randomly
  int x,y,o;
  for(x=0;x<N;x++)for(y=0;y<N;y++)for(o=0;o<2;o++)XB[x][y][o]=randnib();
}

void writeweights(char *f){
  int i,v0,v1;
  FILE *fp;
  fp=fopen(f,"w");assert(fp);
  fprintf(fp,"%d %d %d\n",N,N,wn);
  for(i=0;i<NE;i++){
    v0=elist[i][0];v1=elist[i][2];
    fprintf(fp,"%d %d %d %d   %d %d %d %d   %8d\n",
            decx(v0),decy(v0),deco(v0),deci(v0),
            decx(v1),decy(v1),deco(v1),deci(v1),
            Q[v0][elist[i][1]]);
  }
  fclose(fp);
}

void readweights(char *f){
  int i,j,w,v0,v1,x0,y0,o0,i0,e0,x1,y1,o1,i1,e1,nx,ny;
  FILE *fp;
  printf("Reading weight matrix from file \"%s\"\n",f);
  fp=fopen(f,"r");assert(fp);
  assert(fscanf(fp,"%d %d %d",&nx,&ny,&wn)==3);
  assert(nx==N&&ny==N);
  for(i=0;i<NV;i++)okv[i]=0;
  for(i=0;i<NV;i++)for(j=0;j<6;j++)Q[i][j]=0; // Ensure weight=0 for edges that go out of bounds
  for(i=0;i<NE;i++){
    assert(fscanf(fp,"%d %d %d %d %d %d %d %d %d",
                  &x0,&y0,&o0,&i0,
                  &x1,&y1,&o1,&i1,
                  &w)==9);
    if(x1==x0&&y1==y0){assert(o0!=o1);e0=i1;e1=i0;}else{
      if(abs(x1-x0)==1&&y1==y0&&o0==0&&o1==0){e0=4+(x1-x0+1)/2;e1=9-e0;}else
        if(x1==x0&&abs(y1-y0)==1&&o0==1&&o1==1){e0=4+(y1-y0+1)/2;e1=9-e0;}else assert(0);
    }
    v0=enc(x0,y0,o0,i0);
    v1=enc(x1,y1,o1,i1);
    Q[v0][e0]=Q[v1][e1]=w;
    if(w)okv[v0]=okv[v1]=1;
  }
  fclose(fp);
  getbigweights();
}

int val(void){
  int v,x,y;
  v=0;
  for(x=0;x<N;x++)for(y=0;y<N;y++){
    v+=QB[x][y][0][0][XB[x][y][0]][XB[x][y][1]];
    v+=QB[x][y][0][2][XB[x][y][0]][XB[x+1][y][0]];
    v+=QB[x][y][1][2][XB[x][y][1]][XB[x][y+1][1]];
  }
  return v;
}

void prstate(void){
  static int X0[N][N][2]={{{0}}};
  int nb[16];
  int i,j,o,t,x;
  for(i=1,nb[0]=0;i<16;i++)nb[i]=nb[i>>1]+(i&1);
  for(i=0,t=0;i<N;i++)for(j=0;j<N;j++)for(o=0;o<2;o++)t+=nb[XB[i][j][o]^X0[i][j][o]];
  x=(t>=NV/2?15:0);
  printf("\n");
  for(j=N-1;j>=0;j--){
    for(i=0;i<N;i++)printf(" %X%X",XB[i][j][0]^X0[i][j][0]^x,XB[i][j][1]^X0[i][j][1]^x);
    printf("\n");
  }
  memcpy(X0,XB,sizeof(X0));
}

int opt0(double maxt,int pr){// Simple K_4,4-wise optimisation
  int r,v,x,y,s0,s1,bv,cv,vmin;
  long long int nn;
  double t0,t1,tt;
  bv=1000000000;nn=0;t0=cpu();t1=0;
  do{
    init_state();
    cv=val();
    do{
      for(x=0;x<N;x++)for(y=0;y<N;y++){
        vmin=1000000000;
        for(s0=0;s0<16;s0++)for(s1=0;s1<16;s1++){
          v=QB[x][y][0][0][s0][s1];
          v+=QB[x][y][0][1][s0][XB[x-1][y][0]];
          v+=QB[x][y][0][2][s0][XB[x+1][y][0]];
          v+=QB[x][y][1][1][s1][XB[x][y-1][1]];
          v+=QB[x][y][1][2][s1][XB[x][y+1][1]];
          if(v<vmin){vmin=v;XB[x][y][0]=s0;XB[x][y][1]=s1;}
        }
      }
      v=val();r=(v<cv);if(r)cv=v;
    }while(r);
    nn++;
    tt=cpu()-t0;
    if(cv<bv||tt>=t1||tt>=maxt){
      if(cv<bv)bv=cv;
      if(pr){printf("%12lld %10d %8.2f\n",nn,bv,tt);fflush(stdout);}
      t1=MAX(tt*1.2,tt+5);
    }
  }while(tt<maxt);
  return bv;
}

int lineexhaust(int c,int d){
  // If d=0 exhaust column c, else exhaust row c
  // Comments and variable names are as if in the column case (d=0)
  
  int b,o,r,s,v,smin0,smin1,vmin0,vmin1;
  int v0[16],v1[16];
  int h0[16][N][2],h1[16][N][2];// history

  if(d){
    //return 0;
    for(r=0;r<N;r++){
      for(b=0;b<16;b++){// b = state of (c,r,1)
        if(r>0){
          vmin0=1000000000;smin0=-1;
          for(s=0;s<16;s++){// s = state of (c,r-1,1)
            v=v0[s]+QB[r-1][c][0][2][s][b];
            if(v<vmin0){vmin0=v;smin0=s;}
          }
          memcpy(h1[b],h0[smin0],(2*r-1)*sizeof(int));
          h1[b][r-1][1]=smin0;
        }else vmin0=0;
        vmin1=1000000000;smin1=-1;
        for(s=0;s<16;s++){// s = state of (c,r,0)
          v=QB[r][c][1][0][s][b]+
            QB[r][c][1][1][s][XB[r][c-1][1]]+
            QB[r][c][1][2][s][XB[r][c+1][1]];
          if(v<vmin1){vmin1=v;smin1=s;}
        }
        v1[b]=vmin0+vmin1;
        h1[b][r][0]=smin1;
      }//b
      memcpy(v0,v1,sizeof(v0));
      memcpy(h0,h1,sizeof(h0));
    }//r

    vmin0=1000000000;smin0=-1;
    for(s=0;s<16;s++){// s = state of (c,N-1,1)
      v=v0[s];
      if(v<vmin0){vmin0=v;smin0=s;}
    }
    for(r=0;r<N;r++)for(o=0;o<2;o++)XB[r][c][1-o]=h0[smin0][r][o];
    XB[N-1][c][0]=smin0;
    return vmin0;
  }

  //return 0;
  for(r=0;r<N;r++){
    for(b=0;b<16;b++){// b = state of (c,r,1)
      if(r>0){
        vmin0=1000000000;smin0=-1;
        for(s=0;s<16;s++){// s = state of (c,r-1,1)
          v=v0[s]+QB[c][r-1][1][2][s][b];
          if(v<vmin0){vmin0=v;smin0=s;}
        }
        memcpy(h1[b],h0[smin0],(2*r-1)*sizeof(int));
        h1[b][r-1][1]=smin0;
      }else vmin0=0;
      vmin1=1000000000;smin1=-1;
      for(s=0;s<16;s++){// s = state of (c,r,0)
        v=QB[c][r][0][0][s][b]+
          QB[c][r][0][1][s][XB[c-1][r][0]]+
          QB[c][r][0][2][s][XB[c+1][r][0]];
        if(v<vmin1){vmin1=v;smin1=s;}
      }
      v1[b]=vmin0+vmin1;
      h1[b][r][0]=smin1;
    }//b
    memcpy(v0,v1,sizeof(v0));
    memcpy(h0,h1,sizeof(h0));
  }//r

  vmin0=1000000000;smin0=-1;
  for(s=0;s<16;s++){// s = state of (c,N-1,1)
    v=v0[s];
    if(v<vmin0){vmin0=v;smin0=s;}
  }
  for(r=0;r<N;r++)for(o=0;o<2;o++)XB[c][r][o]=h0[smin0][r][o];
  XB[c][N-1][1]=smin0;
  return vmin0;
}

void shuf(int*a,int n){
  int i,j,t;
  for(i=0;i<n-1;i++){
    j=i+randint(n-i);t=a[i];a[i]=a[j];a[j]=t;
  }
}

int opt1(double maxt,int pr,int opt,double *tts){// Optimisation using line (column/row) exhausts
  int o,r,v,x,bv,cv,ns;
  long long int nn;
  double t0,t1,tt;
  bv=1000000000;nn=0;t0=cpu();t1=0;ns=0;
  do{
    init_state();
    cv=val();
    r=0;
    while(1){
      int i,ord[2*N];
      for(i=0;i<2*N;i++)ord[i]=i;
      //shuf(ord,2*N);
      shuf(ord,N);shuf(ord+N,N);
      for(i=0;i<2*N;i++){
        x=ord[i]%N;o=ord[i]/N;
        lineexhaust(x,o);
        v=val();
        if(v<cv){cv=v;r=0;}else{r+=1;if(r==2*N)goto el0;}
      }
    }
  el0:
    if(pr==2&&cv<=bv){prstate();printf("cv %d\n",cv);}
    nn++;
    tt=cpu()-t0;
    if(cv<bv||tt>=t1||tt>=maxt){
      if(cv<bv)bv=cv;
      t1=MAX(tt*1.2,tt+5);
      if(pr==1){printf("%12lld %10d %8.2f\n",nn,bv,tt);fflush(stdout);}
    }
    if(cv==opt){
      ns++;
      printf("Found %d solution%s from %lld trial%s\n",ns,ns==1?"":"s",nn,nn==1?"":"s");
      fflush(stdout);
    }
    if(ns>=10&&tt>=1)break;
  }while(tt<maxt);
  if(tts)*tts=tt/ns;
  return bv;
}

void initoptions(int ac,char**av){
  int opt;
  wn=NV;infile=outfile=0;seed=time(0);maxt=1e10;statemap[0]=-1;statemap[1]=1;
  bm=-1;
  if(outfile)printf("outfile=%s\n",outfile);
  while((opt=getopt(ac,av,"b:n:o:s:t:x:"))!=-1){
    switch(opt){
    case 'b': bm=atoi(optarg);break;
    case 'n': wn=atoi(optarg);assert(wn<=NV);break;
    case 'o': outfile=strdup(optarg);break;
    case 's': seed=atoi(optarg);break;
    case 't': maxt=atof(optarg);break;
    case 'x': statemap[0]=atoi(optarg);break;
    default:
      fprintf(stderr,"Usage: %s [-n workingnodes] [-o outputprobfile] [-s seed] "
              "[-t maxtime] [-x lowerstatevalue] [inputprobfile]\n",av[0]);
      exit(1);
    }
  }
  if(outfile)printf("outfile=%s\n",outfile);
  if(optind<ac)infile=strdup(av[optind]);
  printf("Working nodes: %d of %d\n",wn,NV);
  printf("Seed: %d\n",seed);
  printf("Max time: %gs\n",maxt);
}

void benchmark(void){
  int gtr[16]={-886,-884,-890,-886,-900,-898,-882,-886,-898,-888,-878,-898,-894,-890,-900,-878};
  char l[100];
  double tts;
  assert(N==8&&bm>=0&&bm<16);
  sprintf(l,"problems/test8_%d",bm);
  readweights(l);
  printf("Ground truth value %d\n",gtr[bm]);
  opt1(10000,1,gtr[bm],&tts);
  printf("Time to solution %g\n",tts);
}

int main(int ac,char**av){
  printf("N=%d\n",N);
  initoptions(ac,av);
  memset(XBplus,0,sizeof(XBplus));
  initrand(seed);
  initgraph();
  if(infile){
    readweights(infile);
  }else{
    initweights();printf("Initialising random weight matrix with %d working node%s\n",wn,wn==1?"":"s");
  }
  printf("%d working node%s\n",wn,wn==1?"":"s");
  printf("States are %d,%d\n",statemap[0],statemap[1]);
  if(outfile)printf("outfile=%s\n",outfile);
  if(outfile){writeweights(outfile);printf("Wrote weight matrix to file \"%s\"\n",outfile);}
  if(bm>=0){benchmark();return 0;}
  opt1(maxt,1,1,0);
  return 0;
}
