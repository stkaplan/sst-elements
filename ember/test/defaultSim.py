

def getWorkFlow( defaults ):
    workFlow = []
    motif = dict.copy( defaults )
    motif['cmd'] = "Init"
    workFlow.append( motif )

    motif = dict.copy( defaults )
    #motif['cmd'] = "Sweep3D nx=30 ny=30 nz=30 computetime=140 pex=4 pey=16 pez=0 kba=10"     
    motif['cmd'] = "SIRIUSTrace traceprefix=/home/tgroves/apps/HPCCG-1.0/test_HPCCG-16.stf"
    workFlow.append( motif )

    motif = dict.copy( defaults )
    motif['cmd'] = "Fini"
    workFlow.append( motif )

	# numNodes = 0 implies use all nodes on network
    numNodes = 0 
    numCores = 1 

    return workFlow, numNodes, numCores 

def getNetwork():

	#platform = 'chamaPSM'
	#platform = 'chamaOpenIB'
	#platform = 'bgq'
	platform = 'default'

	#topo = ''
	#shape = ''
	topo = 'torus'
	shape = '1x4x4'

	return platform, topo, shape 
