dry=(-24.8,-29.7,-15.8)
out={"Engram":(-27.3,-32.1,-19.1),"Callback":(-27.5,-32.0,-18.5),"Retrograde":(-28.9,-33.6,-20.2),
"Updraft":(-28.3,-33.3,-19.2),"Pinhole":(-26.1,-31.9,-22.7),"Afterimage":(-28.0,-33.4,-19.6),
"Murmuration":(-26.7,-32.0,-18.9),"Halation":(-26.7,-31.9,-17.7),"Undertow":(-27.3,-32.1,-18.8),
"Lull":(-28.2,-32.9,-19.3),"Echolalia":(-31.2,-31.3,-24.6),"DejaVu":(-30.4,-31.6,-23.6),
"Kaleido":(-28.7,-30.9,-19.9),"Runaway":(-30.1,-34.9,-20.8)}
allv=[]
for k,v in out.items():
    d=[round(v[i]-dry[i],1) for i in range(3)]; allv+=d
    print(k,d)
print("phrase range",min(round(v[0]-dry[0],1) for v in out.values()),max(round(v[0]-dry[0],1) for v in out.values()))
print("chord range",min(round(v[1]-dry[1],1) for v in out.values()),max(round(v[1]-dry[1],1) for v in out.values()))
print("pad range",min(round(v[2]-dry[2],1) for v in out.values()),max(round(v[2]-dry[2],1) for v in out.values()))
# W-D spreads plucks vs strums
wd={"Echolalia":(-2.9,3.7,-8.3),"DejaVu":(-2.5,3.3,-5.6),"Kaleido":(-2.5,3.1,-2.5),"Pinhole":(2.9,1.7,-10.9)}
for k,v in wd.items(): print(k,"plucks-strums spread",round(abs(v[0]-v[1]),1),"all-3 spread",round(max(v)-min(v),1))
