/***** BEGIN LICENSE BLOCK *****

BSD License

Copyright (c) 2005-2012, NIF File Format Library and Tools
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the NIF File Format Library and Tools project may not be
   used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

***** END LICENCE BLOCK *****/

#include "glview.h"
#include "config.h"
#include "options.h"

#include "nifmodel.h"
#include "gl/glscene.h"
#include "gl/gltex.h"
#include "widgets/fileselect.h"
#include "widgets/floatedit.h"
#include "widgets/floatslider.h"

#include <QActionGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QToolBar>

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLFramebufferObject>
#include <QGLFormat>

// TODO: Determine the necessity of this
// Appears to be used solely for gluErrorString
// There may be some Qt alternative
#ifdef __APPLE__
	#include <OpenGL/glu.h>
#else
	#include <GL/glu.h>
#endif

// TODO: Make these options user configurable.
// TODO: Make this platform independent (Half monitor refresh rate)
#define FPS 60
#define FOV 45.0
#define MOV_SPD 350
#define ROT_SPD 45
#define ZOOM_MIN 1.0
#define ZOOM_MAX 1000.0

#ifndef M_PI
#define M_PI 3.1415926535897932385
#endif


//! \file glview.cpp GLView implementation

GLView * GLView::create()
{
	static QList<QPointer<GLView> > views;

	QGLWidget * share = nullptr;
	for ( const QPointer<GLView>& v : views ) {
		if ( v )
			share = v;
	}

	QGLFormat fmt;
	fmt.setDoubleBuffer( true );
	fmt.setRgba( true );
	fmt.setSamples( Options::antialias() ? 16 : 0 );

	if ( share )
		fmt = share->format();
	else
		fmt.setSampleBuffers( Options::antialias() );

	fmt.setVersion( 2, 1 );
	// Ignored if version < 3.2
	//fmt.setProfile(QGLFormat::CoreProfile);

	views.append( QPointer<GLView>( new GLView( fmt, share ) ) );

	return views.last();
}

GLView::GLView( const QGLFormat & format, const QGLWidget * shareWidget )
	: QGLWidget( format, 0, shareWidget )
{
	// Make the context current on this window
	makeCurrent();

	// Create an OpenGL context
	glContext = context()->contextHandle();

	// Obtain a functions object and resolve all entry points
	glFuncs = glContext->functions();

	if ( !glFuncs ) {
		qWarning( "Could not obtain OpenGL functions" );
		exit( 1 );
	}

	glFuncs->initializeOpenGLFunctions();

	setFocusPolicy( Qt::ClickFocus );
	setAttribute( Qt::WA_NoSystemBackground );
	setAcceptDrops( true );
	setContextMenuPolicy( Qt::CustomContextMenu );

	Zoom = 1.0;
	zInc = 1;

	doCenter  = false;
	doCompile = false;
	doMultisampling = Options::antialias();

	model = nullptr;

	time = 0.0;
	lastTime = QTime::currentTime();

	fpsact = 0.0;
	fpsacc = 0.0;
	fpscnt = 0;

	textures = new TexCache( this );

	scene = new Scene( textures, glContext, glFuncs );
	connect( textures, &TexCache::sigRefresh, this, static_cast<void (GLView::*)()>(&GLView::update) );

	timer = new QTimer( this );
	timer->setInterval( 1000 / FPS );
	timer->start();
	connect( timer, &QTimer::timeout, this, &GLView::advanceGears );


	grpView = new QActionGroup( this );
	grpView->setExclusive( false );
	connect( grpView, &QActionGroup::triggered, this, &GLView::viewAction );

	aViewTop = new QAction( QIcon( ":/btn/viewTop" ), tr( "Top" ), grpView );
	aViewTop->setToolTip( tr( "View from above" ) );
	aViewTop->setCheckable( true );
	aViewTop->setShortcut( Qt::Key_F5 );
	grpView->addAction( aViewTop );

	aViewFront = new QAction( QIcon( ":/btn/viewFront" ), tr( "Front" ), grpView );
	aViewFront->setToolTip( tr( "View from the front" ) );
	aViewFront->setCheckable( true );
	aViewFront->setChecked( true );
	aViewFront->setShortcut( Qt::Key_F6 );
	grpView->addAction( aViewFront );

	aViewSide = new QAction( QIcon( ":/btn/viewSide" ), tr( "Side" ), grpView );
	aViewSide->setToolTip( tr( "View from the side" ) );
	aViewSide->setCheckable( true );
	aViewSide->setShortcut( Qt::Key_F7 );
	grpView->addAction( aViewSide );

	aViewUser = new QAction( QIcon( ":/btn/viewUser" ), tr( "User" ), grpView );
	aViewUser->setToolTip( tr( "Restore the view as it was when Save User View was activated" ) );
	aViewUser->setCheckable( true );
	aViewUser->setShortcut( Qt::Key_F8 );
	grpView->addAction( aViewUser );

	aViewWalk = new QAction( QIcon( ":/btn/viewWalk" ), tr( "Walk" ), grpView );
	aViewWalk->setToolTip( tr( "Enable walk mode" ) );
	aViewWalk->setCheckable( true );
	aViewWalk->setShortcut( Qt::Key_F9 );
	grpView->addAction( aViewWalk );

	aViewFlip = new QAction( QIcon( ":/btn/viewFlip" ), tr( "Flip" ), this );
	aViewFlip->setToolTip( tr( "Flip View from Front to Back, Top to Bottom, Side to Other Side" ) );
	aViewFlip->setCheckable( true );
	aViewFlip->setShortcut( Qt::Key_F11 );
	grpView->addAction( aViewFlip );

	aViewPerspective = new QAction( QIcon( ":/btn/viewPers" ), tr( "Perspective" ), this );
	aViewPerspective->setToolTip( tr( "Perspective View Transformation or Orthogonal View Transformation" ) );
	aViewPerspective->setCheckable( true );
	aViewPerspective->setShortcut( Qt::Key_F10 );
	grpView->addAction( aViewPerspective );

	aViewUserSave = new QAction( tr( "Save User View" ), this );
	aViewUserSave->setToolTip( tr( "Save current view rotation, position and distance" ) );
	aViewUserSave->setShortcut( Qt::CTRL + Qt::Key_F9 );
	connect( aViewUserSave, &QAction::triggered, this, &GLView::sltSaveUserView );

	aPrintView = new QAction( tr( "Save View To File..." ), this );
	connect( aPrintView, &QAction::triggered, this, &GLView::saveImage );

#ifndef QT_NO_DEBUG
	aColorKeyDebug = new QAction( tr( "Color Key Debug" ), this );
	aColorKeyDebug->setCheckable( true );
	aColorKeyDebug->setChecked( false );
#endif

	aAnimate = new QAction( tr( "&Animations" ), this );
	aAnimate->setToolTip( tr( "enables evaluation of animation controllers" ) );
	aAnimate->setCheckable( true );
	aAnimate->setChecked( true );
	connect( aAnimate, &QAction::toggled, this, &GLView::checkActions );
	addAction( aAnimate );

	aAnimPlay = new QAction( QIcon( ":/btn/play" ), tr( "&Play" ), this );
	aAnimPlay->setCheckable( true );
	aAnimPlay->setChecked( true );
	connect( aAnimPlay, &QAction::toggled, this, &GLView::checkActions );

	aAnimLoop = new QAction( QIcon( ":/btn/loop" ), tr( "&Loop" ), this );
	aAnimLoop->setCheckable( true );
	aAnimLoop->setChecked( true );

	aAnimSwitch = new QAction( QIcon( ":/btn/switch" ), tr( "&Switch" ), this );
	aAnimSwitch->setCheckable( true );
	aAnimSwitch->setChecked( true );

	// animation tool bar

	tAnim = new QToolBar( tr( "Animation" ) );
	tAnim->setObjectName( "AnimTool" );
	tAnim->setAllowedAreas( Qt::TopToolBarArea | Qt::BottomToolBarArea );
	tAnim->setIconSize( QSize( 16, 16 ) );

	connect( aAnimate, &QAction::toggled, tAnim->toggleViewAction(), &QAction::setChecked );
	connect( aAnimate, &QAction::toggled, tAnim, &QToolBar::setVisible );
	connect( tAnim->toggleViewAction(), &QAction::toggled, aAnimate, &QAction::setChecked );

	tAnim->addAction( aAnimPlay );

	sldTime = new FloatSlider( Qt::Horizontal, true, true );
	sldTime->setSizePolicy( QSizePolicy::MinimumExpanding, QSizePolicy::Maximum );
	connect( this, &GLView::sigTime, sldTime, &FloatSlider::set );
	connect( sldTime, &FloatSlider::valueChanged, this, &GLView::sltTime );
	tAnim->addWidget( sldTime );

	FloatEdit * edtTime = new FloatEdit;
	edtTime->setSizePolicy( QSizePolicy::Minimum, QSizePolicy::Maximum );

	connect( this, &GLView::sigTime, edtTime, &FloatEdit::set );
	connect( edtTime, static_cast<void (FloatEdit::*)(float)>(&FloatEdit::sigEdited), this, &GLView::sltTime );
	connect( sldTime, &FloatSlider::valueChanged, edtTime, &FloatEdit::setValue );
	connect( edtTime, static_cast<void (FloatEdit::*)(float)>(&FloatEdit::sigEdited), sldTime, &FloatSlider::setValue );
	sldTime->addEditor( edtTime );

	tAnim->addAction( aAnimLoop );

	tAnim->addAction( aAnimSwitch );

	animGroups = new QComboBox();
	animGroups->setMinimumWidth( 100 );
	connect( animGroups, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::activated), this, &GLView::sltSequence );
	tAnim->addWidget( animGroups );

	// TODO: Connect to a less general signal for when texture paths are edited
	//   or things that affect textures.  Not *any* Options update.
	// This makes editing various options sluggish, like lighting color, background color
	//   and even some LineEdits like "Cull Nodes by Name"
	connect( Options::get(), &Options::sigFlush3D, textures, &TexCache::flush );
	connect( Options::get(), &Options::sigChanged, this, static_cast<void (GLView::*)()>(&GLView::update) );
	connect( Options::get(), &Options::materialOverridesChanged, this, &GLView::sceneUpdate );

#ifdef Q_OS_LINUX
	// extra whitespace for linux
	QWidget * extraspace = new QWidget();
	extraspace->setFixedWidth( 5 );
	tAnim->addWidget( extraspace );
#endif

	tView = new QToolBar( tr( "Render View" ) );
	tView->setObjectName( "ViewTool" );
	tView->setAllowedAreas( Qt::TopToolBarArea | Qt::BottomToolBarArea );
	tView->setIconSize( QSize( 16, 16 ) );

	tView->addAction( aViewTop );
	tView->addAction( aViewFront );
	tView->addAction( aViewSide );
	tView->addAction( aViewUser );
	tView->addAction( aViewWalk );
	tView->addSeparator();
	tView->addAction( aViewFlip );
	tView->addAction( aViewPerspective );

#ifdef Q_OS_LINUX
	// extra whitespace for linux
	extraspace = new QWidget();
	extraspace->setFixedWidth( 5 );
	tView->addWidget( extraspace );
#endif
}

GLView::~GLView()
{
	delete scene;
}


/* 
 * Scene
 */

Scene * GLView::getScene()
{
	return scene;
}

void GLView::sceneUpdate()
{
	scene->update( model, QModelIndex() );
	update();
}


/*
 *  OpenGL
 */

void GLView::initializeGL()
{
	GLenum err;
	
	if ( Options::antialias() ) {
		if ( !glContext->hasExtension( "GL_EXT_framebuffer_multisample" ) ) {
			doMultisampling = false;
			//qWarning() << "System does not support multisampling";
		} /* else {
			GLint maxSamples;
			glGetIntegerv( GL_MAX_SAMPLES, &maxSamples );
			qDebug() << "Max samples:" << maxSamples;
		}*/
	}

	initializeTextureUnits( glContext );

	if ( scene->renderer->initialize() )
		updateShaders();

	// Check for errors
	while ( ( err = glGetError() ) != GL_NO_ERROR )
		qDebug() << tr( "glview.cpp - GL ERROR (init) : " ) << (const char *)gluErrorString( err );
}

void GLView::updateShaders()
{
	makeCurrent();
	scene->updateShaders();
	update();
}

void GLView::glProjection( int x, int y )
{
	Q_UNUSED( x ); Q_UNUSED( y );

	GLint viewport[4];
	glGetIntegerv( GL_VIEWPORT, viewport );
	GLdouble aspect = (GLdouble)viewport[2] / (GLdouble)viewport[3];

	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();

	BoundSphere bs = scene->view * scene->bounds();

	if ( Options::drawAxes() )
		bs |= BoundSphere( scene->view * Vector3(), axis );

	GLdouble nr = fabs( bs.center[2] ) - bs.radius * 1.2;
	GLdouble fr = fabs( bs.center[2] ) + bs.radius * 1.2;

	if ( aViewPerspective->isChecked() || aViewWalk->isChecked() ) {
		// Perspective View
		if ( nr < 1.0 )
			nr = 1.0;
		if ( fr < 2.0 )
			fr = 2.0;

		if ( nr > fr ) {
			// add: swap them when needed
			GLfloat tmp = nr;
			nr = fr;
			fr = tmp;
		}

		if ( (fr - nr) < 0.00001f ) {
			// add: ensure distance
			nr = 1.0;
			fr = 2.0;
		}

		GLdouble h2 = tan( ( FOV / Zoom ) / 360 * M_PI ) * nr;
		GLdouble w2 = h2 * aspect;
		glFrustum( -w2, +w2, -h2, +h2, nr, fr );
	} else {
		// Orthographic View
		GLdouble h2 = Dist / Zoom;
		GLdouble w2 = h2 * aspect;
		glOrtho( -w2, +w2, -h2, +h2, nr, fr );
	}

	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
}

#ifdef USE_GL_QPAINTER
void GLView::paintEvent( QPaintEvent * event )
{
	makeCurrent();

	QPainter painter;
	painter.begin( this );
	painter.setRenderHint( QPainter::TextAntialiasing );
#else
void GLView::paintGL()
{
#endif
	// Save GL state
	glPushAttrib( GL_ALL_ATTRIB_BITS );
	glMatrixMode( GL_PROJECTION );
	glPushMatrix();
	glMatrixMode( GL_MODELVIEW );
	glPushMatrix();

	// Clear Viewport
	glViewport( 0, 0, width(), height() );
	qglClearColor( Options::bgColor() );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	// Compile the model
	if ( doCompile ) {
		textures->setNifFolder( model->getFolder() );
		scene->make( model );
		scene->transform( Transform(), scene->timeMin() );
		axis = scene->bounds().radius < 0 ? 1 : scene->bounds().radius * 1.4; // fix: the axis appearance when there is no scene yet

		if ( axis == 0 )
			axis = 1;

		if ( time < scene->timeMin() || time > scene->timeMax() )
			time = scene->timeMin();

		emit sigTime( time, scene->timeMin(), scene->timeMax() );

		animGroups->clear();
		animGroups->addItems( scene->animGroups );
		animGroups->setCurrentIndex( scene->animGroups.indexOf( scene->animGroup ) );

		doCompile = false;
	}

	// Center the model
	if ( doCenter ) {
		viewAction( checkedViewAction() );
		doCenter = false;
	}

	// Transform the scene
	Matrix ap;
	if ( Options::upAxis() == Options::YAxis ) {
		ap( 0, 0 ) = 0; ap( 0, 1 ) = 0; ap( 0, 2 ) = 1;
		ap( 1, 0 ) = 1; ap( 1, 1 ) = 0; ap( 1, 2 ) = 0;
		ap( 2, 0 ) = 0; ap( 2, 1 ) = 1; ap( 2, 2 ) = 0;
	} else if ( Options::upAxis() == Options::XAxis ) {
		ap( 0, 0 ) = 0; ap( 0, 1 ) = 1; ap( 0, 2 ) = 0;
		ap( 1, 0 ) = 0; ap( 1, 1 ) = 0; ap( 1, 2 ) = 1;
		ap( 2, 0 ) = 1; ap( 2, 1 ) = 0; ap( 2, 2 ) = 0;
	}

	Transform viewTrans;
	viewTrans.rotation.fromEuler( Rot[0] / 180.0 * PI, Rot[1] / 180.0 * PI, Rot[2] / 180.0 * PI );
	viewTrans.rotation = viewTrans.rotation * ap;
	viewTrans.translation = viewTrans.rotation * Pos;

	if ( !aViewWalk->isChecked() )
		viewTrans.translation[2] -= Dist * 2;

	scene->transform( viewTrans, time );

	// Setup projection mode
	glProjection();
	glLoadIdentity();

	// Draw the axes
	if ( Options::drawAxes() ) {
		glDisable( GL_ALPHA_TEST );
		glDisable( GL_BLEND );
		glDisable( GL_LIGHTING );
		glDisable( GL_COLOR_MATERIAL );
		glEnable( GL_DEPTH_TEST );
		glDepthMask( GL_TRUE );
		glDepthFunc( GL_LESS );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_NORMALIZE );
		glLineWidth( 2.0f );

		glPushMatrix();
		glLoadMatrix( viewTrans );

		drawAxes( Vector3(), axis );

		glPopMatrix();
	}

	// Setup light
	Vector4 lightDir( 0.0, 0.0, 1.0, 0.0 );

	if ( !Options::lightFrontal() ) {
		float decl = Options::lightDeclination() / 180.0 * PI;
		Vector3 v( sin( decl ), 0, cos( decl ) );
		Matrix m; m.fromEuler( 0, 0, Options::lightPlanarAngle() / 180.0 * PI );
		v = m * v;
		lightDir = Vector4( viewTrans.rotation * v, 0.0 );
	}

	glShadeModel( GL_SMOOTH );
	glLightfv( GL_LIGHT0, GL_POSITION, lightDir.data() );
	glLightfv( GL_LIGHT0, GL_AMBIENT, Color4( Options::ambient() ).data() );
	glLightfv( GL_LIGHT0, GL_DIFFUSE, Color4( Options::diffuse() ).data() );
	glLightfv( GL_LIGHT0, GL_SPECULAR, Color4( Options::specular() ).data() );
	glLightModeli( GL_LIGHT_MODEL_TWO_SIDE, GL_TRUE );
	glEnable( GL_LIGHT0 );
	glEnable( GL_LIGHTING );

	if ( Options::antialias() && doMultisampling )
		glEnable( GL_MULTISAMPLE_ARB );

	// Initialize Rendering Font
	// TODO: Seek alternative to fontDisplayListBase or determine if code is actually necessary
	//glListBase(fontDisplayListBase(QFont(), 2000));

#ifndef QT_NO_DEBUG
	// Color Key debug
	if ( aColorKeyDebug->isChecked() ) {
		glDisable( GL_MULTISAMPLE );
		glDisable( GL_LINE_SMOOTH );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_BLEND );
		glDisable( GL_DITHER );
		glDisable( GL_LIGHTING );
		glShadeModel( GL_FLAT );
		glDisable( GL_FOG );
		glDisable( GL_MULTISAMPLE_ARB );
		glEnable( GL_DEPTH_TEST );
		glDepthFunc( GL_LEQUAL );
		glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
		Node::SELECTING = 1;
	} else {
		Node::SELECTING = 0;
	}
#endif

	// Draw the model
	scene->draw();

	// Restore GL state
	glPopAttrib();
	glMatrixMode( GL_MODELVIEW );
	glPopMatrix();
	glMatrixMode( GL_PROJECTION );
	glPopMatrix();

	// Check for errors
	GLenum err;
	while ( ( err = glGetError() ) != GL_NO_ERROR )
		qDebug() << tr( "glview.cpp - GL ERROR (paint): " ) << (const char *)gluErrorString( err );
	
	// Update FPS counter
	if ( fpsacc > 1.0 && fpscnt ) {
		fpsacc /= fpscnt;

		if ( fpsacc > 0.0001 )
			fpsact = 1.0 / fpsacc;
		else
			fpsact = 10000;

		fpsacc = 0;
		fpscnt = 0;
	}

	emit paintUpdate();

#ifdef USE_GL_QPAINTER
	// draw text on top using QPainter

	if ( Options::benchmark() || Options::drawStats() ) {
		int ls = QFontMetrics( font() ).lineSpacing();
		int y  = 1;

		painter.setPen( Options::hlColor() );

		if ( Options::benchmark() ) {
			painter.drawText( 10, y++ *ls, QString( "FPS %1" ).arg( int(fpsact) ) );
			y++;
		}

		if ( Options::drawStats() ) {
			QString stats = scene->textStats();
			QStringList lines = stats.split( "\n" );
			for ( const QString& line : lines ) {
				painter.drawText( 10, y++ *ls, line );
			}
		}
	}

	painter.end();
#endif
}

void GLView::resizeGL( int width, int height )
{
	glViewport( 0, 0, width, height );
}

typedef void (Scene::* DrawFunc)( void );

int indexAt( /*GLuint *buffer,*/ NifModel * model, Scene * scene, QList<DrawFunc> drawFunc, int cycle, const QPoint & pos, int & furn )
{
	Q_UNUSED( model ); Q_UNUSED( cycle );
	// Color Key O(1) selection
	//	Open GL 3.0 says glRenderMode is deprecated
	//	ATI OpenGL API implementation of GL_SELECT corrupts NifSkope memory
	//
	// Create FBO for sharp edges and no shading.
	// Texturing, blending, dithering, lighting and smooth shading should be disabled.
	// The FBO can be used for the drawing operations to keep the drawing operations invisible to the user.

	GLint viewport[4];
	glGetIntegerv( GL_VIEWPORT, viewport );

	// Create new FBO with multisampling disabled
	QOpenGLFramebufferObjectFormat fboFmt;
	fboFmt.setTextureTarget( GL_TEXTURE_2D );
	fboFmt.setInternalTextureFormat( GL_RGB32F_ARB );
	fboFmt.setAttachment( QOpenGLFramebufferObject::Attachment::CombinedDepthStencil );

	QOpenGLFramebufferObject fbo( viewport[2], viewport[3], fboFmt );
	fbo.bind();

	glEnable( GL_LIGHTING );
	glDisable( GL_MULTISAMPLE );
	glDisable( GL_MULTISAMPLE_ARB );
	glDisable( GL_LINE_SMOOTH );
	glDisable( GL_POINT_SMOOTH );
	glDisable( GL_POLYGON_SMOOTH );
	glDisable( GL_TEXTURE_1D );
	glDisable( GL_TEXTURE_2D );
	glDisable( GL_TEXTURE_3D );
	glDisable( GL_BLEND );
	glDisable( GL_DITHER );
	glDisable( GL_FOG );
	glDisable( GL_LIGHTING );
	glShadeModel( GL_FLAT );
	glEnable( GL_DEPTH_TEST );
	glDepthFunc( GL_LEQUAL );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );

	// Rasterize the scene
	Node::SELECTING = 1;
	for ( DrawFunc df : drawFunc ) {
		(scene->*df)();
	}
	Node::SELECTING = 0;

	fbo.release();

	QImage img( fbo.toImage() );
	QColor pixel = img.pixel( pos );

#ifndef QT_NO_DEBUG
	img.save( "fbo.png" );
#endif

	// Encode RGB to Int
	int a = 0;
	a |= pixel.red()   << 0;
	a |= pixel.green() << 8;
	a |= pixel.blue()  << 16;

	// Decode:
	// R = (id & 0x000000FF) >> 0
	// G = (id & 0x0000FF00) >> 8
	// B = (id & 0x00FF0000) >> 16

	int choose = COLORKEY2ID( a );

	// Pick BSFurnitureMarker
	if ( choose > 0 ) {
		auto furnBlock = model->getBlock( model->index( 3, 0, model->getBlock( choose & 0x0ffff ) ), "BSFurnitureMarker" );

		if ( furnBlock.isValid() ) {
			furn = choose >> 16;
			choose &= 0x0ffff;
		}
	}

	//qDebug() << "Key:" << a << " R" << pixel.red() << " G" << pixel.green() << " B" << pixel.blue();
	return choose;
}

QModelIndex GLView::indexAt( const QPoint & pos, int cycle )
{
	if ( !(model && isVisible() && height()) )
		return QModelIndex();

	makeCurrent();

	glPushAttrib( GL_ALL_ATTRIB_BITS );
	glMatrixMode( GL_PROJECTION );
	glPushMatrix();
	glMatrixMode( GL_MODELVIEW );
	glPushMatrix();

	glViewport( 0, 0, width(), height() );
	glProjection( pos.x(), pos.y() );

	QList<DrawFunc> df;

	if ( Options::drawHavok() )
		df << &Scene::drawHavok;

	if ( Options::drawNodes() )
		df << &Scene::drawNodes;

	if ( Options::drawFurn() )
		df << &Scene::drawFurn;

	df << &Scene::drawShapes;

	int choose = -1, furn = -1;
	choose = ::indexAt( model, scene, df, cycle, pos, /*out*/ furn );

	glPopAttrib();
	glMatrixMode( GL_MODELVIEW );
	glPopMatrix();
	glMatrixMode( GL_PROJECTION );
	glPopMatrix();

	QModelIndex chooseIndex;

	if ( choose != -1 ) {
		// Block Index
		chooseIndex = model->getBlock( choose );

		if ( furn != -1 ) {
			// Furniture Row @ Block Index
			chooseIndex = model->index( furn, 0, model->index( 3, 0, chooseIndex ) );
		}			
	}

	return chooseIndex;
}

void GLView::center()
{
	doCenter = true;
	update();
}

void GLView::move( float x, float y, float z )
{
	Pos += Matrix::euler( Rot[0] / 180 * PI, Rot[1] / 180 * PI, Rot[2] / 180 * PI ).inverted() * Vector3( x, y, z );
	update();
}

void GLView::rotate( float x, float y, float z )
{
	Rot += Vector3( x, y, z );
	uncheckViewAction();
	update();
}

void GLView::zoom( float z )
{
	Zoom *= z;

	if ( Zoom < ZOOM_MIN )
		Zoom = ZOOM_MIN;

	if ( Zoom > ZOOM_MAX )
		Zoom = ZOOM_MAX;

	update();
}

void GLView::setDistance( float x )
{
	Dist = x;
	update();
}

void GLView::setPosition( float x, float y, float z )
{
	Pos = { x, y, z };
	update();
}

void GLView::setPosition( Vector3 v )
{
	Pos = v;
	update();
}

void GLView::setRotation( float x, float y, float z )
{
	Rot = { x, y, z };
	update();
}

void GLView::setZoom( float z )
{
	Zoom = z;
	update();
}

/*
 *  NifModel
 */

void GLView::setNif( NifModel * nif )
{
	if ( model ) {
		// disconnect( model ) may not work with new Qt5 syntax...
		// it says the calls need to remain symmetric to the connect() ones.
		// Otherwise, use QMetaObject::Connection
		disconnect( model );
	}

	model = nif;

	if ( model ) {
		connect( model, &NifModel::dataChanged, this, &GLView::dataChanged );
		connect( model, &NifModel::linksChanged, this, &GLView::modelLinked );
		connect( model, &NifModel::modelReset, this, &GLView::modelChanged );
		connect( model, &NifModel::destroyed, this, &GLView::modelDestroyed );
	}

	doCompile = true;
}

void GLView::setCurrentIndex( const QModelIndex & index )
{
	if ( !( model && index.model() == model ) )
		return;

	scene->currentBlock = model->getBlock( index );
	scene->currentIndex = index.sibling( index.row(), 0 );

	update();
}

QModelIndex parent( QModelIndex ix, QModelIndex xi )
{
	ix = ix.sibling( ix.row(), 0 );
	xi = xi.sibling( xi.row(), 0 );

	while ( ix.isValid() ) {
		QModelIndex x = xi;

		while ( x.isValid() ) {
			if ( ix == x )
				return ix;

			x = x.parent();
		}

		ix = ix.parent();
	}

	return QModelIndex();
}

void GLView::dataChanged( const QModelIndex & idx, const QModelIndex & xdi )
{
	if ( doCompile )
		return;

	QModelIndex ix = idx;

	if ( idx == xdi ) {
		if ( idx.column() != 0 )
			ix = idx.sibling( idx.row(), 0 );
	} else {
		ix = ::parent( idx, xdi );
	}

	if ( ix.isValid() ) {
		scene->update( model, idx );
		update();
	} else {
		modelChanged();
	}
}

void GLView::modelChanged()
{
	doCompile = true;
	doCenter  = true;
	update();
}

void GLView::modelLinked()
{
	doCompile = true; //scene->update( model, QModelIndex() );
	update();
}

void GLView::modelDestroyed()
{
	setNif( 0 );
}


/*
 * UI
 */

QMenu * GLView::createMenu() const
{
	QMenu * m = new QMenu( tr( "&Render" ) );
	m->addAction( aViewTop );
	m->addAction( aViewFront );
	m->addAction( aViewSide );
	m->addAction( aViewWalk );
	m->addAction( aViewUser );
	m->addSeparator();
	m->addAction( aViewFlip );
	m->addAction( aViewPerspective );
	m->addAction( aViewUserSave );
	m->addSeparator();
	m->addAction( aPrintView );
#ifndef QT_NO_DEBUG
	m->addAction( aColorKeyDebug );
#endif
	m->addSeparator();
	m->addActions( Options::actions() );
	return m;
}

QList<QToolBar *> GLView::toolbars() const
{
	return{ tView, tAnim };
}

void GLView::viewAction( QAction * act )
{
	BoundSphere bs = scene->bounds();

	if ( Options::drawAxes() )
		bs |= BoundSphere( Vector3(), axis );

	if ( bs.radius < 1 )
		bs.radius = 1;

	setDistance( bs.radius );
	setZoom( 1.0 );

	if ( act == aViewWalk ) {
		setRotation( -90, 0, 0 );
		setPosition( Vector3() - scene->bounds().center );
		setZoom( 1.0 );
		aViewWalk->setChecked( true );
		aViewTop->setChecked( false );
		aViewFront->setChecked( false );
		aViewSide->setChecked( false );
		aViewUser->setChecked( false );
	}

	if ( !act || act == aViewFlip ) {
		act = checkedViewAction();
	}

	if ( act != aViewWalk )
		setPosition( Vector3() - bs.center );

	if ( act == aViewTop ) {
		if ( aViewFlip->isChecked() )
			setRotation( 180, 0, 0 );
		else
			setRotation( 0, 0, 0 );

		aViewWalk->setChecked( false );
		aViewTop->setChecked( true );
		aViewFront->setChecked( false );
		aViewSide->setChecked( false );
		aViewUser->setChecked( false );
	} else if ( act == aViewFront ) {
		if ( aViewFlip->isChecked() )
			setRotation( -90, 0, 180 );
		else
			setRotation( -90, 0, 0 );

		aViewWalk->setChecked( false );
		aViewTop->setChecked( false );
		aViewFront->setChecked( true );
		aViewSide->setChecked( false );
		aViewUser->setChecked( false );
	} else if ( act == aViewSide ) {
		if ( aViewFlip->isChecked() )
			setRotation( -90, 0, -90 );
		else
			setRotation( -90, 0, 90 );

		aViewWalk->setChecked( false );
		aViewTop->setChecked( false );
		aViewFront->setChecked( false );
		aViewSide->setChecked( true );
		aViewUser->setChecked( false );
	} else if ( act == aViewUser ) {
		QSettings cfg;
		cfg.beginGroup( "GLView" );
		cfg.beginGroup( "User View" );
		setRotation( cfg.value( "RotX" ).toDouble(), cfg.value( "RotY" ).toDouble(), cfg.value( "RotZ" ).toDouble() );
		setPosition( cfg.value( "PosX" ).toDouble(), cfg.value( "PosY" ).toDouble(), cfg.value( "PosZ" ).toDouble() );
		setDistance( cfg.value( "Dist" ).toDouble() );
		aViewWalk->setChecked( false );
		aViewTop->setChecked( false );
		aViewFront->setChecked( false );
		aViewSide->setChecked( false );
		aViewUser->setChecked( true );
		cfg.endGroup();
		cfg.endGroup();
	}

	update();
}

void GLView::sltTime( float t )
{
	time = t;
	update();
	emit sigTime( time, scene->timeMin(), scene->timeMax() );
}

void GLView::sltSequence( const QString & seqname )
{
	animGroups->setCurrentIndex( scene->animGroups.indexOf( seqname ) );
	scene->setSequence( seqname );
	time = scene->timeMin();
	emit sigTime( time, scene->timeMin(), scene->timeMax() );
	update();
}

void GLView::sltSaveUserView()
{
	QSettings cfg;
	cfg.beginGroup( "GLView" );
	cfg.beginGroup( "User View" );
	cfg.setValue( "RotX", Rot[0] );
	cfg.setValue( "RotY", Rot[1] );
	cfg.setValue( "RotZ", Rot[2] );
	cfg.setValue( "PosX", Pos[0] );
	cfg.setValue( "PosY", Pos[1] );
	cfg.setValue( "PosZ", Pos[2] );
	cfg.setValue( "Dist", Dist );
	viewAction( aViewUser );
	cfg.endGroup();
	cfg.endGroup();
}

QAction * GLView::checkedViewAction() const
{
	if ( aViewTop->isChecked() )
		return aViewTop;
	else if ( aViewFront->isChecked() )
		return aViewFront;
	else if ( aViewSide->isChecked() )
		return aViewSide;
	else if ( aViewWalk->isChecked() )
		return aViewWalk;
	else if ( aViewUser->isChecked() )
		return aViewUser;

	return 0;
}

void GLView::uncheckViewAction()
{
	QAction * act = checkedViewAction();

	if ( act && act != aViewWalk )
		act->setChecked( false );
}

void GLView::advanceGears()
{
	QTime t  = QTime::currentTime();
	float dT = lastTime.msecsTo( t ) / 1000.0;

	if ( Options::benchmark() ) {
		fpsacc += dT;
		fpscnt++;
		update();
	}

	if ( dT < 0 )
		dT = 0;

	if ( dT > 1.0 )
		dT = 1.0;

	lastTime = t;

	if ( !isVisible() )
		return;

	if ( aAnimate->isChecked() && aAnimPlay->isChecked() && scene->timeMin() != scene->timeMax() ) {
		time += dT;

		if ( time > scene->timeMax() ) {
			if ( aAnimSwitch->isChecked() && !scene->animGroups.isEmpty() ) {
				int ix = scene->animGroups.indexOf( scene->animGroup );

				if ( ++ix >= scene->animGroups.count() )
					ix -= scene->animGroups.count();

				sltSequence( scene->animGroups.value( ix ) );
			} else if ( aAnimLoop->isChecked() ) {
				time = scene->timeMin();
			}
		}

		emit sigTime( time, scene->timeMin(), scene->timeMax() );
		update();
	}

	// Rotation
	if ( kbd[ Qt::Key_Up ] )    rotate( -ROT_SPD * dT, 0, 0 );
	if ( kbd[ Qt::Key_Down ] )  rotate( +ROT_SPD * dT, 0, 0 );
	if ( kbd[ Qt::Key_Left ] )  rotate( 0, 0, -ROT_SPD * dT );
	if ( kbd[ Qt::Key_Right ] ) rotate( 0, 0, +ROT_SPD * dT );

	// Movement
	if ( kbd[ Qt::Key_A ] ) move( +MOV_SPD * dT, 0, 0 );
	if ( kbd[ Qt::Key_D ] ) move( -MOV_SPD * dT, 0, 0 );
	if ( kbd[ Qt::Key_W ] ) move( 0, 0, +MOV_SPD * dT );
	if ( kbd[ Qt::Key_S ] ) move( 0, 0, -MOV_SPD * dT );
	if ( kbd[ Qt::Key_F ] ) move( 0, +MOV_SPD * dT, 0 );
	if ( kbd[ Qt::Key_R ] ) move( 0, -MOV_SPD * dT, 0 );

	// Zoom
	if ( kbd[ Qt::Key_Q ] ) setDistance( Dist * 1.0 / 1.1 );
	if ( kbd[ Qt::Key_E ] ) setDistance( Dist * 1.1 );

	// Focal Length
	if ( kbd[ Qt::Key_PageUp ] )   zoom( 1.1f );
	if ( kbd[ Qt::Key_PageDown ] ) zoom( 1 / 1.1f );

	if ( mouseMov[0] != 0 || mouseMov[1] != 0 || mouseMov[2] != 0 ) {
		move( mouseMov[0], mouseMov[1], mouseMov[2] );
		mouseMov = Vector3();
	}

	if ( mouseRot[0] != 0 || mouseRot[1] != 0 || mouseRot[2] != 0 ) {
		rotate( mouseRot[0], mouseRot[1], mouseRot[2] );
		mouseRot = Vector3();
	}
}

void GLView::checkActions()
{
	scene->animate = aAnimate->isChecked();

	lastTime = QTime::currentTime();

	if ( Options::benchmark() )
		timer->setInterval( 0 );
	else
		timer->setInterval( 1000 / FPS );

	update();
}


/*
 * Settings
 */

void GLView::save( QSettings & settings )
{
	settings.setValue( "GLView/Enable Animations", aAnimate->isChecked() );
	settings.setValue( "GLView/Play Animation", aAnimPlay->isChecked() );
	settings.setValue( "GLView/Loop Animation", aAnimLoop->isChecked() );
	settings.setValue( "GLView/Switch Animation", aAnimSwitch->isChecked() );
	settings.setValue( "GLView/Perspective", aViewPerspective->isChecked() );

	if ( checkedViewAction() )
		settings.setValue( "GLView/View Action", checkedViewAction()->text() );
}

void GLView::restore( const QSettings & settings )
{
	aAnimate->setChecked( settings.value( "GLView/Enable Animations", true ).toBool() );
	aAnimPlay->setChecked( settings.value( "GLView/Play Animation", true ).toBool() );
	aAnimLoop->setChecked( settings.value( "GLView/Loop Animation", true ).toBool() );
	aAnimSwitch->setChecked( settings.value( "GLView/Switch Animation", true ).toBool() );
	aViewPerspective->setChecked( settings.value( "GLView/Perspective", true ).toBool() );
	checkActions();
	QString viewAct = settings.value( "GLView/View Action", "Front" ).toString();
	for ( QAction * act : grpView->actions() ) {
		if ( act->text() == viewAct )
			viewAction( act );
	}
}

void GLView::saveImage()
{
	QDialog dlg;
	QGridLayout * lay = new QGridLayout;
	dlg.setWindowTitle( tr( "Save View" ) );
	dlg.setLayout( lay );
	dlg.setMinimumWidth( 400 );

	QString date = QDateTime::currentDateTime().toString( "yyyyMMdd_HH-mm-ss" );
	QString name = model->getFilename();

	QString nifFolder = model->getFolder();
	QString filename = name + (!name.isEmpty() ? "_" : "") + date + ".jpg";

	// Default: NifSkope directory
	// TODO: User-configurable default screenshot path in Options
	QString nifskopePath = "screenshots/" + filename;
	// Absolute: NIF directory
	QString nifPath = nifFolder + (!nifFolder.isEmpty() ? "/" : "") + filename;

	FileSelector * file = new FileSelector( FileSelector::SaveFile, tr( "File" ), QBoxLayout::LeftToRight );
	file->setFilter( { "Images (*.jpg *.png *.bmp)", "JPEG (*.jpg)", "PNG (*.png)", "BMP (*.bmp)" } );
	file->setFile( nifskopePath );
	lay->addWidget( file, 0, 0, 1, -1 );
	
	QRadioButton * nifskopeDir = new QRadioButton( tr( "NifSkope Directory" ), this );
	nifskopeDir->setChecked( true );
	nifskopeDir->setToolTip( tr( "Save to NifSkope screenshots directory" ) );

	QRadioButton * niffileDir = new QRadioButton( tr( "NIF Directory" ), this );
	niffileDir->setChecked( false );
	niffileDir->setDisabled( nifFolder.isEmpty() );
	niffileDir->setToolTip( tr( "Save to NIF file directory" ) );

	lay->addWidget( nifskopeDir, 1, 0, 1, 1 );
	lay->addWidget( niffileDir, 1, 1, 1, 1 );

	// Save JPEG Quality
	QSettings cfg;
	int jpegQuality = cfg.value( "JPEG/Quality", 91 ).toInt();
	cfg.setValue( "JPEG/Quality", jpegQuality );

	QHBoxLayout * pixBox = new QHBoxLayout;
	pixBox->setAlignment( Qt::AlignRight );
	QSpinBox * pixQuality = new QSpinBox;
	pixQuality->setRange( -1, 100 );
	pixQuality->setSingleStep( 10 );
	pixQuality->setValue( jpegQuality );
	pixQuality->setSpecialValueText( tr( "Auto" ) );
	pixQuality->setMaximumWidth( pixQuality->minimumSizeHint().width() );
	pixBox->addWidget( new QLabel( tr( "JPEG Quality" ) ) );
	pixBox->addWidget( pixQuality );
	lay->addLayout( pixBox, 1, 2, Qt::AlignRight );

	QHBoxLayout * hBox  = new QHBoxLayout;
	QPushButton * btnOk = new QPushButton( tr( "Save" ) );
	QPushButton * btnCancel = new QPushButton( tr( "Cancel" ) );
	hBox->addWidget( btnOk );
	hBox->addWidget( btnCancel );
	lay->addLayout( hBox, 2, 0, 1, -1 );

	// Set FileSelector to NifSkope dir (relative)
	connect( nifskopeDir, &QRadioButton::clicked, [&]()
		{
			file->setText( nifskopePath );
			file->setFile( nifskopePath );
		}
	);
	// Set FileSelector to NIF File dir (absolute)
	connect( niffileDir, &QRadioButton::clicked, [&]()
		{
			file->setText( nifPath );
			file->setFile( nifPath );
		}
	);

	// Validate on OK
	connect( btnOk, &QPushButton::clicked, [&]() 
		{
			// Save JPEG Quality
			cfg.setValue( "JPEG/Quality", pixQuality->value() );

			// TODO: Set up creation of screenshots directory in Options
			if ( nifskopeDir->isChecked() ) {
				QDir workingDir;
				workingDir.mkpath( "screenshots" );
			}

			QImage img = grabFrameBuffer();
			if ( img.save( file->file(), 0, pixQuality->value() ) ) {
				dlg.accept();
			} else {
				qWarning() << "Could not save to file. Please check the filepath and extension are valid.";
			}
		}
	);
	connect( btnCancel, &QPushButton::clicked, &dlg, &QDialog::reject );

	if ( dlg.exec() != QDialog::Accepted )
		return;
}


/* 
 * QWidget Event Handlers 
 */

void GLView::dragEnterEvent( QDragEnterEvent * e )
{
	if ( e->mimeData()->hasUrls() && e->mimeData()->urls().count() == 1 ) {
		QUrl url = e->mimeData()->urls().first();

		if ( url.scheme() == "file" ) {
			QString fn = url.toLocalFile();

			if ( textures->canLoad( fn ) ) {
				fnDragTex = textures->stripPath( fn, model->getFolder() );
				e->accept();
				return;
			}
		}
	}

	e->ignore();
}

void GLView::dragLeaveEvent( QDragLeaveEvent * e )
{
	Q_UNUSED( e );

	if ( iDragTarget.isValid() ) {
		model->set<QString>( iDragTarget, fnDragTexOrg );
		iDragTarget = QModelIndex();
		fnDragTex = fnDragTexOrg = QString();
	}
}

void GLView::dragMoveEvent( QDragMoveEvent * e )
{
	if ( iDragTarget.isValid() ) {
		model->set<QString>( iDragTarget, fnDragTexOrg );
		iDragTarget  = QModelIndex();
		fnDragTexOrg = QString();
	}

	QModelIndex iObj = model->getBlock( indexAt( e->pos() ), "NiAVObject" );

	if ( iObj.isValid() ) {
		for ( const auto l : model->getChildLinks( model->getBlockNumber( iObj ) ) ) {
			QModelIndex iTxt = model->getBlock( l, "NiTexturingProperty" );

			if ( iTxt.isValid() ) {
				QModelIndex iSrc = model->getBlock( model->getLink( iTxt, "Base Texture/Source" ), "NiSourceTexture" );

				if ( iSrc.isValid() ) {
					iDragTarget = model->getIndex( iSrc, "File Name" );

					if ( iDragTarget.isValid() ) {
						fnDragTexOrg = model->get<QString>( iDragTarget );
						model->set<QString>( iDragTarget, fnDragTex );
						e->accept();
						return;
					}
				}
			}
		}
	}

	e->ignore();
}

void GLView::dropEvent( QDropEvent * e )
{
	iDragTarget = QModelIndex();
	fnDragTex = fnDragTexOrg = QString();
	e->accept();
}

void GLView::focusOutEvent( QFocusEvent * )
{
	kbd.clear();
}

void GLView::keyPressEvent( QKeyEvent * event )
{
	switch ( event->key() ) {
	case Qt::Key_Up:
	case Qt::Key_Down:
	case Qt::Key_Left:
	case Qt::Key_Right:
	case Qt::Key_PageUp:
	case Qt::Key_PageDown:
	case Qt::Key_A:
	case Qt::Key_D:
	case Qt::Key_W:
	case Qt::Key_S:
	case Qt::Key_R:
	case Qt::Key_F:
	case Qt::Key_Q:
	case Qt::Key_E:
		kbd[event->key()] = true;
		break;
	case Qt::Key_Escape:
		doCompile = true;

		if ( !aViewWalk->isChecked() )
			doCenter = true;

		update();
		break;
	case Qt::Key_C:
	{
		Node * node = scene->getNode( model, scene->currentBlock );

		if ( node != 0 ) {
			BoundSphere bs = node->bounds();

			this->setPosition( -bs.center );

			if ( bs.radius > 0 ) {
				setDistance( bs.radius * 1.5f );
			}
		}
	}
		break;
	default:
		event->ignore();
		break;
	}
}

void GLView::keyReleaseEvent( QKeyEvent * event )
{
	switch ( event->key() ) {
	case Qt::Key_Up:
	case Qt::Key_Down:
	case Qt::Key_Left:
	case Qt::Key_Right:
	case Qt::Key_PageUp:
	case Qt::Key_PageDown:
	case Qt::Key_A:
	case Qt::Key_D:
	case Qt::Key_W:
	case Qt::Key_S:
	case Qt::Key_R:
	case Qt::Key_F:
	case Qt::Key_Q:
	case Qt::Key_E:
		kbd[event->key()] = false;
		break;
	default:
		event->ignore();
		break;
	}
}

void GLView::mouseDoubleClickEvent( QMouseEvent * )
{
	/*
	doCompile = true;
	if ( ! aViewWalk->isChecked() )
	doCenter = true;
	update();
	*/
}

void GLView::mouseMoveEvent( QMouseEvent * event )
{
	int dx = event->x() - lastPos.x();
	int dy = event->y() - lastPos.y();

	if ( event->buttons() & Qt::LeftButton ) {
		mouseRot += Vector3( dy * .5, 0, dx * .5 );
	} else if ( event->buttons() & Qt::MidButton ) {
		float d = axis / (qMax( width(), height() ) + 1);
		mouseMov += Vector3( dx * d, -dy * d, 0 );
	} else if ( event->buttons() & Qt::RightButton ) {
		setDistance( Dist - (dx + dy) * (axis / (qMax( width(), height() ) + 1)) );
	}

	lastPos = event->pos();
}

void GLView::mousePressEvent( QMouseEvent * event )
{
	lastPos = event->pos();

	if ( (pressPos - event->pos()).manhattanLength() <= 3 )
		cycleSelect++;
	else
		cycleSelect = 0;

	pressPos = event->pos();
}

void GLView::mouseReleaseEvent( QMouseEvent * event )
{
	if ( !(model && (pressPos - event->pos()).manhattanLength() <= 3) )
		return;

	QModelIndex idx = indexAt( event->pos(), cycleSelect );
	scene->currentBlock = model->getBlock( idx );
	scene->currentIndex = idx.sibling( idx.row(), 0 );

	if ( idx.isValid() ) {
		emit clicked( idx );
	}

	update();
}

void GLView::wheelEvent( QWheelEvent * event )
{
	if ( aViewWalk->isChecked() )
		mouseMov += Vector3( 0, 0, event->delta() );
	else
		setDistance( Dist * (event->delta() < 0 ? 1.0 / 0.8 : 0.8) );
}
