#!/usr/bin/env python3
"""Reference re-implementation of the DynoData engine model (EngineModel__buildTables 0x4f4640 / EngineModel__stepCrank 0x4f50e0).
See notes/10-engine-dyno.md. Usage: dyno_model.py  (prints rpm, torque[Nm], brake hp for the DynoData.java defaults)."""
import math
def dyno(cyl=4,bore=0.082,stroke=0.090,Vmin=0.00005,in_min=0.005,out_min=0.005,in_max=0.8,out_max=0.8,
         t_in_open=0.001,t_in_close=0.25,t_out_open=0.75,t_out_close=0.99,
         spark_min=0.48,spark_inc=-0.05,spark_rpm0=2000.,spark_rpm1=5000.,time_burn=0.003,
         mixture_H=1e6,mixture_ratio=14.,turbo_mul=0.,turbo_opt=0.,turbo_range=0.,P_turbo_max=0.,P_waste=None,
         intercooling=1.0,max_fuel=0.,max_air=0.,maxRPM=0.,steps=28):
    L=0.15; r=stroke*0.5; R=bore*0.5; A=math.pi*R*R; Vd=stroke*A; Vtot=Vd+Vmin
    rho0=1.2929; Pamb=101325.; Pex=111457.5; cv=712.; Twall=353.15; Tloss=95.
    Tin0=293.15*intercooling; Tex=293.15
    rho_in0=Pamb*9.869233e-06*rho0; rho_ex=Pex*9.869233e-06*rho0
    fuelfrac=1/(mixture_ratio+1); airfrac=fuelfrac*mixture_ratio
    if P_waste is None: P_waste=P_turbo_max
    if maxRPM<=0: maxRPM=(45.0/r)*9.549296
    step=max(int(maxRPM/steps),1); count=int(maxRPM)//step
    tq=[0.0]; hp=[0.0]; fuel_rate=0.0; air_rate=0.0
    for i in range(1,count):
        rpm=float(i*step); Tc=120.0/rpm; w=rpm*0.10471976; dt=Tc*0.0043290043
        # setupRpmPoint
        d=spark_rpm1-spark_rpm0; f=(rpm-spark_rpm0)/d if d>0 else -1
        spark=spark_min if f<0 else spark_min+spark_inc*min(f,1.0)
        Pin=Pamb; Hin=mixture_H; Tin=Tin0; rho_in=rho_in0
        if turbo_mul>0 and turbo_range>0:
            x=abs(rpm*turbo_mul-turbo_opt); g=1.0 if x<0 else max(0.0,1-x/turbo_range)
            b=min(P_turbo_max*g,P_waste); s=math.sqrt(b+1)
            Tin*=s; rho_in*=s; Pin*=s*s
        if 0<max_fuel<fuel_rate: Hin=Hin/(fuel_rate/max_fuel)
        if 0<max_air<air_rate: Pin=Pin/(air_rate/max_air)  # setupRpmPoint writes +0x158 = P_in (tasks/007)
        burn=(1-min(rpm*5e-5,0.9))*time_burn
        # resetCycle (once per rpm point); the state then carries over between cycles (runCycle only clears accumulators)
        S=dict(P=Pin,T=Tin,H=Hin,t=0.,th=0.,b=0.,W=0.,V=Vmin,m=Vmin*rho_in)
        def cycle():
            P=S['P'];T=S['T'];H=S['H'];t=S['t'];th=S['th'];b=S['b'];W=S['W'];V=S['V'];m=S['m']
            fuel=0.;air=0.;ncyc=0;res=None
            while ncyc==0:
                Vprev=max(V,0.); P0=P; t+=dt
                if t>=Tc:
                    ncyc+=1; t-=Tc; Wprev=W; W=0.
                    imep=Wprev/Vd if Vd>1e-8 else 0.
                    torque=cyl*imep*Vd*0.125; pind=rpm*imep*Vd*0.016666668*cyl
                    res=[imep,torque,pind,pind-rpm**3*2e-8]
                th+=w*dt
                if th>12.566371: th-=12.566371
                xx=math.sqrt(L*L-(r*math.sin(th))**2)+math.cos(th)*r
                V=((r+L)-xx)*A+Vmin
                if V<1e-7: V=1e-7; continue
                q=Vprev/V; P*=q**1.3; T*=q**0.3
                def inwin(o,c):
                    if c<=o: return t>o*Tc or t<c*Tc
                    return o*Tc<t<c*Tc
                ain=in_max if inwin(t_in_open,t_in_close) else in_min
                aout=out_max if inwin(t_out_open,t_out_close) else out_min
                ts=spark*Tc; te=burn+ts
                if te>Tc: te-=Tc
                bp=b
                if te>ts: inside = ts<t<=te
                else: inside = t>ts or t<=te
                if inside: b=(t-ts)/burn
                elif bp>0: b=1.0
                else: b=0.0
                if b>0:
                    mo=m; m=max(m,1e-7); T=max(T,1e-7)
                    Tn=(b-bp)*H*mo/(cv*m)+T; P*=Tn/T; T=Tn
                    if b>=1: b=0.; H=0.
                for (a,Pt,rt,Tt,Ht,clamp) in ((ain,Pin,rho_in,Tin,Hin,True),(aout,Pex,rho_ex,Tex,0.0,False)):
                    if a<=0: continue
                    k=min(a*dt*1000.,1.); Pn=P+k*(Pt-P)
                    if clamp: Pn=max(Pn,1e-6)
                    Veq=V*P/Pn
                    if V<=Veq: m-=(Veq-V)/V*m
                    else:
                        dm=(Pn/Pt)*rt*(V-Veq); m=max(m,1e-7); mn=m+dm
                        T=(Tt*dm+T*m)/mn; H=(Ht*dm+H*m)/mn
                        if clamp: air+=dm*airfrac; fuel+=dm*fuelfrac
                        m=mn
                    m=max(m,0.); P=Pn
                kk=min(dt/Tloss,1.); dd=T-Twall
                loss=dd*dd*(1 if dd>0 else -1 if dd<0 else 0)*kk
                if dd!=0 and loss/dd>1: loss=dd
                Tn=T-loss; P*=Tn/max(T,1e-7); T=Tn
                W+=(P+P0)*0.5*(V-Vprev)
            S.update(P=P,T=T,H=H,t=t,th=th,b=b,W=W,V=V,m=m)
            res.append(fuel*cyl/Tc); res.append(air*cyl/Tc)   # kg/s, used by the next rpm point's limits
            return res
        r1=cycle(); ref=r1[0]
        for it in range(10):
            r1=cycle()
            if r1[0]!=0 and abs(ref/r1[0]-1)<0.05: break
            ref=ref*0.4+r1[0]*0.6
        fuel_rate,air_rate=r1[4],r1[5]
        tq.append(r1[1]); hp.append(r1[3])
    return step,tq,hp
if __name__=='__main__':
    step,tq,hp=dyno(maxRPM=7000,steps=28)
    for i,(a,b) in enumerate(zip(tq,hp)): print(i*step, round(a,1), round(b*0.001341,1))
