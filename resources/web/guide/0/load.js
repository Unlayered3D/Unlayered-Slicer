
var TargetPage=null;
var WaitDetail=false;

function OnInit()
{
	TranslatePage();

	TargetPage=GetQueryString("target");

	/* ORCA the filament pages need the stage-2 profile data (machine/filament/process), which
	   Studio loads after the printer models. Hold the spinner here until it is in, rather than
	   letting the target page render an empty filament list. Studio broadcasts the finish
	   message when stage 2 lands; this ping covers the case where it landed already. */
	WaitDetail = ( GetQueryString("wait")=="detail" );
	if( WaitDetail )
	{
		var tSend={};
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="request_userguide_detail_status";

		SendWXMessage( JSON.stringify(tSend) );
	}
	
	// Orca: fallback timeout in case the C++ -> JS signal fails (e.g., WebKit issues).
	// Jump to the target page after 3 minutes so slow computers don't get stuck on a partially loaded page.
	setTimeout("JumpToTarget()",180*1000);

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="userguide_loading_page_ready";
	SendWXMessage(JSON.stringify(tSend));
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];
	
	if(WaitDetail)
	{
		if(strCmd=='userguide_profile_detail_load_finish')
			JumpToTarget();
	}
	else if(strCmd=='userguide_profile_load_finish')
	{
		JumpToTarget();
	}
}

function JumpToTarget()
{
	window.open('../'+TargetPage+'/index.html','_self');
}