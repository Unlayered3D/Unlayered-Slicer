// Unlayered 0.1.7: the "Sign in to Unlayered" step (see index.html).

var m_Busy=false;
var m_Left=false;
// Unlayered 0.1.8: opened from "Update your Unlayered printer" (guide 21update) to sign in for the
// printer settings; Next, Skip and Back all return there.
var m_From=GetQueryString("from");

function OnInit()
{
	TranslatePage();
	Send({'command':'request_signin_state'});
}

function Send( obj )
{
	obj['sequence_id']=Math.round(new Date() / 1000);
	SendWXMessage( JSON.stringify(obj) );
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];

	if( strCmd=='signin_state' )
	{
		$('#SignInTitle').text(pVal['title']);
		$('#SignInText').text(pVal['text']);
		$('#SignInBtn').text(pVal['button']);
		$('#SkipLink').text(pVal['skip']);
		ShowState(pVal);
	}
	else if( strCmd=='signin_result' )
	{
		m_Busy=false;
		$('#SignInBtn').removeClass('Busy');
		ShowState(pVal);
		// Signed in: on to the next step, as if Next had been pressed.
		if( pVal['signed_in'] )
			setTimeout(GoNext, 1200);
	}
	else if( strCmd=='page_after_signin' || strCmd=='page_before_signin' )
	{
		Leave(pVal['page']);
	}
}

function ShowState( pVal )
{
	let signedIn=!!pVal['signed_in'];
	$('#SignInBtn').prop('hidden', signedIn);
	$('#SkipLink').prop('hidden', signedIn);
	$('#NextBtn').prop('hidden', !signedIn);
	$('#SignedInText').text(pVal['signed_in_text'] || '').prop('hidden', !signedIn);
	let failed=pVal['failed_text'] || '';
	$('#FailedText').text(failed).prop('hidden', signedIn || failed=='');
}

function DoSignIn()
{
	if( m_Busy )
		return;
	m_Busy=true;
	$('#SignInBtn').addClass('Busy');
	$('#FailedText').prop('hidden', true);
	Send({'command':'account_sign_in'});
}

function GoNext()
{
	if( m_Busy || m_Left )
		return;
	if( m_From=='21update' )
	{
		Leave('21update');
		return;
	}
	Send({'command':'request_page_after_signin'});
	// Should no answer come, the printer picker anyway.
	setTimeout(function(){ Leave('21'); }, 15000);
}

function GoBack()
{
	if( m_Busy || m_Left )
		return;
	Leave(m_From=='21update' ? '21update' : '13');
}

function Leave( page )
{
	if( m_Left )
		return;
	m_Left=true;
	let known={'13':1,'5import':1,'21':1,'21update':1};
	window.location.href="../"+(known[page] ? page : '21')+"/index.html";
}
