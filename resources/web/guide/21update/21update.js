// Unlayered 0.1.8: the "Update your Unlayered printer" step (see index.html).

var m_Busy=false;
var m_Left=false;

function OnInit()
{
	TranslatePage();
	Send({'command':'request_printer_update_state'});
}

function Send( obj )
{
	obj['sequence_id']=Math.round(new Date() / 1000);
	SendWXMessage( JSON.stringify(obj) );
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];

	if( strCmd=='printer_update_state' )
	{
		// Not signed in: the configuration feed needs the account; firmware does not.
		$('#SignInNote').prop('hidden', !!pVal['signed_in']);
		ShowPrinter(pVal['printer'] || '');
		m_Busy=!!pVal['busy'];
		$('#CheckBtn').toggleClass('Busy', m_Busy);
		ShowResult(pVal['result'] || '', !!pVal['checked']);
	}
	else if( strCmd=='printer_update_checking' )
	{
		m_Busy=true;
		$('#CheckBtn').addClass('Busy');
		ShowPrinter(pVal['printer'] || '');
		ShowResult(pVal['text'] || '', false);
	}
	else if( strCmd=='printer_update_result' )
	{
		m_Busy=false;
		$('#CheckBtn').removeClass('Busy');
		ShowResult(pVal['text'] || '', true);
	}
}

function ShowPrinter( text )
{
	$('#PrinterText').text(text).prop('hidden', text=='');
}

// After a check, Next replaces Do it later: the job is done, or handed to the slicer for later.
function ShowResult( text, checked )
{
	$('#ResultText').text(text).prop('hidden', text=='');
	$('#LaterBtn').prop('hidden', checked);
	$('#NextBtn').prop('hidden', !checked);
}

function DoCheck()
{
	if( m_Busy )
		return;
	m_Busy=true;
	$('#CheckBtn').addClass('Busy');
	Send({'command':'check_printer_update'});
	// A check is a few seconds; should no answer ever come, let the button work again.
	setTimeout(function(){ m_Busy=false; $('#CheckBtn').removeClass('Busy'); }, 60000);
}

// The wizard's own sign-in step; it comes back here (13signin.js, from=21update).
function GoSignIn()
{
	Leave('../13signin/index.html?from=21update');
}

// A check still running does not hold the wizard: its answer is dropped with the page, and the
// slicer has already kept the flag for the check after the wizard.
function GoNext()
{
	Leave('../0/index.html?target=22&wait=detail');
}

function GoBack()
{
	Leave('../21/index.html');
}

function Leave( url )
{
	if( m_Left )
		return;
	m_Left=true;
	window.location.href=url;
}
