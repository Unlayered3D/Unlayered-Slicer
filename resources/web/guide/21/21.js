// UNIQUE FUNCTIONS

// Keep in here for future additions
function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	RequestProfile();
}
 
// Unlayered 0.1.7: Back goes to the "Settings from other slicers" step (guide 5import) when the
// wizard showed it, else to the privacy page; the slicer knows which (HandleStudio, page_before_models).
function GoBackFromModels()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_page_before_models";

	SendWXMessage( JSON.stringify(tSend) );
}

// Unlayered 0.1.8: Next asks the slicer whether the "Update your Unlayered printer" step (guide
// 21update) comes first - only when an Unlayered printer is ticked or already set up. The answer is
// page_after_models (21/common.js). The selection is saved first (OnExitFilter), so it counts.
var m_LeftModels=false;

function GotoFilamentPage()
{
	let nChoose=OnExitFilter();
	
	if(nChoose>0)
	{
		var tSend={};
		tSend['sequence_id']=Math.round(new Date() / 1000);
		tSend['command']="request_page_after_models";
		SendWXMessage( JSON.stringify(tSend) );
		// Should no answer come, the filament page anyway.
		setTimeout(function(){ LeaveModels('22'); }, 15000);
	}
}

function LeaveModels( page )
{
	if( m_LeftModels )
		return;
	m_LeftModels=true;
	if( page=='21update' )
		window.open('../21update/index.html','_self');
	else
		window.open('../0/index.html?target=22&wait=detail','_self');
}
