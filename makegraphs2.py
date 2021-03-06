#!/usr/bin/python

# Make Boixo-style graphs showing dependence on size of problem and
# stratified by difficulty percentile.

# We're trying to find an estimator for the distribution percentiles from a finite number of observations.
# E.g., p-point of the distribution from a sample of 1000. Call the answer T_p and the estimator T_p^*.
# Order the observations: t_1 <= t_2 <= ... <= t_{1000}, so t_i is the i^th order statistic
# P(t_i <= T_p < t_{i+1}) = p^i(1-p)^(1000-i)(1000 choose i) = P(B(1000,p)=i), so P(T_p < t_i) = P(B(1000,p)<i)
# ^ is true over the prob space of random (t_i). Assuming a improper uniform prior for T_p, you can view
# this as a posterior density on T_p, and you can take the median to get a reasonably robust estimator, T_p^*
# that doesn't usually depend on the distribution <t_1 or >t_{1000}, provided 1000 is big enough and
# we're not asking for too extreme a p. The procedure is then:
#
# Given (t_i) and p, find unique i such that
# P(B(1000,p)<i) <= 0.5 < P(B(1000,p)<=i)
# then let T_p^* = ((0.5-P(B(1000,p)<i))*t_{i+1}+(P(B(1000,p)<=i)-0.5)*t_i)/P(B(1000,p)=i).
#
# To estimate V[T_p^*], let s(a,b,x) = E[(X-x)^2] if X ~ U[a,b], then take
# sum_{j=0}^{1000} P(B(1000,p)=j)*s(t_j,t_{j+1},T_p^*)
# If this puts too much weight on the unknowns t_0 and t_{1001} then we're in a bit of trouble,
# but don't expect this to happen if 1000*min(p,1-p) isn't too small.
# Can put t_0=t_1/10, t_{1001}=10*t_{1000}, or something, but we can't be absolutely sure
# this is conservative enough.
#
# The above is actually applied to log_10(time)

import os,sys
from math import log,exp,sqrt

from subprocess import Popen,PIPE
from optparse import OptionParser
parser = OptionParser(usage="usage: %prog [options]")
parser.add_option("-?","--wtf",action="help",help="Show this help message and exit")
parser.add_option("-o","--outformat",dest="outformat",default="png",help="Output format, png or ps (default: png)")
parser.add_option("-O","--outfile",dest="outfile",default=None,help="Instead of running gnuplot, output gnuplot commands to this file (default: None)")
parser.add_option("-s","--super",action="store_true",default=False,dest="super",help="Final graph is to be superimposed (vs standalone)")
parser.add_option("-p","--percentiles",dest="percentiles",default="[50,90]",help="List of percentiles, or \"average\"")
parser.add_option("-c","--certainty",dest="certainty",default="TTS",help="Certainty (e.g., 0.99) (only in superimpose mode)")
parser.add_option("-n","--cores",dest="cores",default="1",help="Number of cores to assume")
parser.add_option("-m","--min",dest="min",default="",help="Values to be minned, e.g., 'S3/S4,S13/S14'")

(opts,args)=parser.parse_args()
outformat=opts.outformat
standalone=not opts.super
percentiles=eval(opts.percentiles)
certainty="TTS" if (standalone or opts.certainty=="TTS") else float(opts.certainty)
if opts.certainty!="TTS" and standalone: print>>sys.stderr,"Warning: reverting to TTS because not in superimpose mode"
cores=int(opts.cores)
print "Output format:",outformat
print "Standalone flag:",standalone
print "Percentiles:",percentiles
print "Certainty:",certainty
print "Cores:",cores

weightmodes=[(7,"Range_1"),(11,"Range_7")]
outdir='sizegraphs'
colpc={1:0x0000ff, 5:0x004488, 10:0x00ffff, 50:0x00ff00,
       75:0x888800, 90:0xff8800, 95:0xff0000, 99:0x000000,
       'average':0x8000ff}# Match colours from Boixo papers in superimpose mode (function of percentile)
colseq=[0xff8800, 0x000000, 0x00ffff, 0x8000ff, 0x888800, 0x0000ff, 0xff0000, 0x00ff00, 0x004488]
def include(N,S):# Whether to include this (size,strat) pair
  if N==439 or N==502: return 0# Ignore legacy sizes to avoid clutter and keep to complete grids
  #return N>8 and (S[:2]=='DW' or S=='S13' or S=='S14' or S=='PPT-T1' or S=='PPT-T2')
  return N>8 and (S[:2]=='DW' or S=='S3' or S=='S4' or S=='S13' or S=='S14' or S=='PPT-T1' or S=='PPT-T2')
  return S[:6]=='emctts' or S=='S13' or S=='S14' or S=='PPT-T1' or S=='PPT-T2'
  return S=='S13'
  #return (N<8*14*14 and S=='S13') or (N>=8*14*14 and S=='S14')
  return S=='S13' or S=='S14'
  #return 1
  return S in [3,13]
  if standalone: return (N<=800 and S==3) or (N>=512 and N<=1152 and S==13)
  return (N<512 and S==3) or (N>=512 and N<=1152 and S==13)

log10=log(10)
try: os.makedirs(outdir)
except OSError: pass

for (wm,wname) in weightmodes:
  d={}# d maps (percentile, strat) to (size, est time at that percentile, sd at that percentile, num samples)
  msd=[]
  dir0='output/weightmode%d'%wm
  for x in os.listdir(dir0):
    # Looking for directories with names like output/weightmode7/512.strat3
    # or output/weightmode7/2048.emctts-T1-tlogt
    f=x.find('.')
    if f==-1 or not x[:f].isdigit(): continue
    N=int(x[:f])
    if x[f+1:f+6]=='strat': S='S'+x[f+6:]
    elif x[f+1:f+7]=='emctts': S=x[f+1:]
    elif x[f+1:f+4]=='PPT': S=x[f+1:]
    elif x[f+1:f+3]=='DW': S=x[f+1:]
    else: continue
    if not include(N,S): continue
    fn=os.path.join(dir0,x,'summary')
    print "Processing file",fn
    fp=open(fn,'r')
    if x[f+1:f+3]=='DW':
      # D-Wave summary file contains ready-made percentile values, in the form:
      # Percentile  Certainty  log_10(Time/us)  Error
      for y in fp:
        if y=="" or y[0]=='#': continue
        (pc,c,t,sd)=[float(w) for w in y.split()]
        if pc in percentiles:
          t-=log(log(1/(1-c)))/log10+6# Convert from certainty form (us) into TTS (s)
          t+=log(1. if certainty=='TTS' else log(1/(1-certainty)))/log10# Possibly convert back to certainty form
          d.setdefault((pc,S),[]).append((N,t,sd,1))# Using fictitious number (1) of samples
      fp.close();continue
    l=[];l0=[]
    for y in fp:
      if y=="" or y[0]=='#': continue
      z=float(y.split()[2])
      l0.append(z);l.append(log(z)/log10)
    fp.close()
    l.sort();n=len(l)
    if n>1:
      mu=sum(l)/n;sd=sqrt(sum([(x-mu)**2 for x in l])/(n*(n-1)))
      msd.append((wname,N,S,n,mu,sd))
    #if n!=1000: print >>sys.stderr,"Warning: %d samples in summary file %s"%(n,fn)
    for pc in percentiles:
      if pc=='average':
        assert n>1
        t=sum(l0)/n;sd=sqrt(sum([(x-t)**2 for x in l0])/(n*(n-1)))
        sd=(log(t+sd)-log(t))/log10
        t=log(t)/log10
      else:
        p=pc/100.;lp=log(p);lq=log(1-p)
        # Interpolating the p-point of the values l[0], ..., l[n-1] as in the comment at the start
        s=0;pr=n*lq
        for i in range(n+1):# Find median point of (n choose i)*p^i*(1-p)^(n-i)
          p1=exp(pr);s+=p1# pr=log( (n choose i)*p^i*(1-p)^(n-i) )
          if s>=0.5: break
          pr+=lp-lq+log((n-i)/(i+1.))
        if i==0 or i==n: print >>sys.stderr,"Warning: Too few datapoints (%d) in summary file %s for %g%% point"%(n,fn,pc);continue
        t=((s-.5)*l[i-1]+(.5-(s-p1))*l[i])/p1# t = interpolated value of log(time)
        # Now estimating variance in the estimator to be able to plot error bars
        v=0;pr=n*lq
        def s(a,b,t): return ((a-t)**2+(a-t)*(b-t)+(b-t)**2)/3
        for i in range(n+1):
          p1=exp(pr)
          a=l[i-1] if i>0 else l[0]-1
          b=l[i] if i<n else l[n-1]+1
          v+=p1*s(a,b,t)
          if i==n: break
          pr+=lp-lq+log((n-i)/(i+1.))
        sd=sqrt(v)
      t+=log((1. if certainty=='TTS' else log(1/(1-certainty)))/cores)/log10
      d.setdefault((pc,S),[]).append((N,t,sd,n))
  
  if opts.min!="":
    for m in opts.min.split(','):
      lm=m.split('/')
      ld=list(d)
      for (pc,S) in ld:
        if (pc,S) not in d: continue
        if S in lm:
          bv=None
          for y in lm:
            k=(pc,y)
            if k in d:
              if bv==None: bv=d[k]
              else:# bv=pointwisemin(bv,d[k])
                e={}
                for (N,t,sd,n) in bv+d[k]:
                  if N not in e or t<e[N][1]: e[N]=(N,t,sd,n)
                bv=e.values();bv.sort()
              del d[k]
          d[(pc,m)]=bv[:]

  msd.sort()
  fp=open(outdir+'/'+wname+'-msd.txt','w')
  print >>fp,"     wname     N   S n_samp mean(log10(t)) stderr(log10(t))"
  for (wname1,N,S,n,mu,sd) in msd:
    print >>fp,"%10s %5d %10s %6d         %6.2f          %7.3f"%(wname1,N,S,n,mu,sd)
  fp.close()

  fp=open(outdir+'/'+wname+'.txt','w')
  print >>fp,"     wname     N   S n_samp %-tile     log10(t) sd(log10(t))"
  for (pc,S) in sorted(list(d)):
    d[(pc,S)].sort()
    for (N,t,sd,n) in d[(pc,S)]:
      print >>fp,"%10s %5d %10s %6d %s %12g %12g"%(wname,N,S,n,pc if pc=='average' else "%7.1f"%pc,t,sd)
    print >>fp
  fp.close()
  
  if opts.outfile: p=open(opts.outfile+'_'+wname,'w')
  else: p=Popen("gnuplot",shell=True,stdin=PIPE).stdin
  if outformat=='ps':
    print >>p,'set terminal postscript color solid "Helvetica" 9'
  elif outformat=='png':
    print >>p,'set terminal pngcairo font "sans,16" size 1400,960'
    print >>p,'set bmargin 5;set lmargin 15;set rmargin 15;set tmargin 5'
  else: assert 0
  ofn="%s/%s%s.%s"%(outdir,wname,"" if standalone else "-tobemerged",outformat)
  print >>p,'set output "%s"'%ofn
  #print >>p,'set zeroaxis'
  #print >>p,'set xrange [0:101]'
  if standalone: print >>p,'set key left'
  else: print >>p,'set key center top'
  print >>p,'set title "%s / prog-qubo"'%(wname.replace('_',' '))
  print >>p,'set xlabel "Problem size, N, spaced linearly with sqrt(N)"'
  if standalone: print >>p,'set ylabel "log_10(TTS in s), %d core%s"'%(cores,"s"*(cores!=1))
  print >>p,'set y2tics mirror'
  print >>p,'set grid ytics lc rgb "#dddddd" lt 1'
  s='plot ';cn=0
  for (pc,S) in sorted(list(d)):
    if standalone: col=colseq[cn]
    else: col=colpc[pc]
    if s!='plot ': s+=', '
    s+='"-" using (sqrt($1)):2:3:xticlabels(1) with yerrorbars lt rgb "#%06x" title "%s-%s", '%(col,pc if pc=='average' else '%d%%'%pc,S)
    s+='"-" using (sqrt($1)):2:3:xticlabels(1) with lines lt rgb "#%06x" notitle'%col
    cn+=1
  print >>p,s
  for (pc,S) in sorted(list(d)):
    for i in range(2):
      for (N,t,sd,n) in d[(pc,S)]:
        print >>p,"%d %g %g"%(N,t+(0 if standalone else 6),sd)
      print >>p,"e"
  p.close()
  print "Written graph to %s"%ofn
