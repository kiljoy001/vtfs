</$objtype/mkfile>

TARG=vtfs
OFILES=vtfs.$O

$TARG: $OFILES
	$LD -o $TARG $OFILES /$objtype/lib/lib9p.a

