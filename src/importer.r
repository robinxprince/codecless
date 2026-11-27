// Mac importers are identified by an 'IMPT' resource. The value is a unique four-character
// code After Effects requires; it isn't related to the file types the importer opens.
type 'IMPT'
{
	longint;
};

resource 'IMPT' (1000)
{
	0x4D456176	// 'MEav'
};
