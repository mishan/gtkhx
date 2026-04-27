#define TYPE_cicn	0x6369636e

typedef void *Ptr;
typedef void **Handle;
typedef guint32 Fixed;

struct Rect {
	guint16 top;	/* upper boundary of rectangle */
	guint16 left;	/* left boundary of rectangle */
	guint16 bottom;	/* lower boundary of rectangle */
	guint16 right;	/* right boundary of rectangle */
};

typedef struct Rect Rect;

struct RGBColor {
	guint16 red;	/* magnitude of red component */
	guint16 green;	/* magnitude of green component */
	guint16 blue;	/* magnitude of blue component */
};

typedef struct RGBColor RGBColor;

struct ColorSpec {
	guint16 value;	/* index or other value */
	RGBColor rgb;	/* true color */
};

typedef struct ColorSpec ColorSpec;
typedef ColorSpec *ColorSpecPtr;

typedef ColorSpec CSpecArray[1];

struct ColorTable {
	guint32 ctSeed PACKED;		/* unique identifier for table */
	guint16 ctFlags PACKED;		/* high bit: 0 = PixMap; 1 = device */
	guint16 ctSize PACKED;		/* number of entries in CTTable */
	CSpecArray ctTable PACKED;	/* array [0..0] of ColorSpec */
};

typedef struct ColorTable ColorTable;
typedef ColorTable *CTabPtr, **CTabHandle;

struct PixMap {
	Ptr baseAddr PACKED;		/* pointer to pixels */
	guint16 rowBytes PACKED;		/* offset to next line */
	Rect bounds PACKED;		/* encloses bitmap */
	guint16 pmVersion PACKED;	/* pixMap version number */
	guint16 packType PACKED;		/* defines packing format */
	guint32 packSize PACKED;		/* length of pixel data */
	Fixed hRes PACKED;		/* horiz. resolution (ppi) */
	Fixed vRes PACKED;		/* vert. resolution (ppi) */
	guint16 pixelType PACKED;	/* defines pixel type */
	guint16 pixelSize PACKED;	/* # bits in pixel */
	guint16 cmpCount PACKED;		/* # components in pixel */
	guint16 cmpSize PACKED;		/* # bits per component */
	guint32 planeBytes PACKED;	/* offset to next plane */
	CTabHandle pmTable PACKED;	/* color map for this pixMap */
	guint32 pmReserved PACKED;	/* for future use. MUST BE 0 */
};

typedef struct PixMap PixMap;
typedef PixMap *PixMapPtr, **PixMapHandle;

struct BitMap {
	Ptr baseAddr PACKED;
	guint16 rowBytes PACKED;
	Rect bounds PACKED;
};

typedef struct BitMap BitMap;
typedef BitMap *BitMapPtr, **BitMapHandle;

struct CIcon {
	PixMap iconPMap;	/* the icon's pixMap */
	BitMap iconMask;	/* the icon's mask */
	BitMap iconBMap;	/* the icon's bitMap */
	Handle iconData;	/* the icon's data */
	guint16 iconMaskData[1];	/* icon's mask and BitMap data */
};

typedef struct CIcon CIcon;
typedef CIcon *CIconPtr, **CIconHandle;

/*

struct cicn_resource {
	icon's pixel map;		// 50 bytes
	mask bitmap;			// 14
	icon's bitmap;			// 14
	icon's data;			// 4
	mask bitmap image data;		// variable
	icon's bitmap image data;	// variable
	icon's color table;		// variable
	pixel map image data;		// variable
};
*/

struct ifn;

void load_icon (GtkWidget *widget, guint16 icon, struct ifn *ifn, char recurse, GdkPixmap **pixmap, GdkBitmap **bitmap);
extern GdkImage *cicn_to_gdkimage (GdkColormap *colormap, GdkVisual *visual, void *cicn_rsrc, unsigned int len, GdkImage **maskimp);

