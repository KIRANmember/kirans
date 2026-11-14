(() => {
  const configReady = () => window.PILLMATE_SUPABASE_URL && window.PILLMATE_SUPABASE_ANON_KEY && !window.PILLMATE_SUPABASE_URL.includes("YOUR_") && !window.PILLMATE_SUPABASE_ANON_KEY.includes("YOUR_");
  let client;
  const getClient = () => {
    if (!configReady()) throw new Error("PillMate authentication is not connected yet. Supabase project settings need to be added.");
    if (!window.supabase?.createClient) throw new Error("Authentication library did not load. Please refresh and try again.");
    return client ||= window.supabase.createClient(window.PILLMATE_SUPABASE_URL, window.PILLMATE_SUPABASE_ANON_KEY);
  };
  const form = document.querySelector("[data-auth-form]");
  if (!form) return;
  const message = document.querySelector("[data-message]");
  const submit = form.querySelector('button[type="submit"]');
  const showMessage = (text, type="error") => { message.textContent=text; message.className="message show "+type; };
  const setBusy = (busy) => { submit.disabled=busy; submit.textContent=busy ? "Please wait…" : submit.dataset.label; };
  document.querySelectorAll("[data-toggle-password]").forEach(button => button.addEventListener("click", () => {
    const input=document.getElementById(button.dataset.togglePassword);
    input.type=input.type==="password"?"text":"password";
    button.textContent=input.type==="password"?"Show":"Hide";
  }));
  form.addEventListener("submit", async event => {
    event.preventDefault(); message.className="message"; setBusy(true);
    try {
      const sb=getClient(), data=Object.fromEntries(new FormData(form).entries());
      const mode=form.dataset.authForm;
      if(mode==="signup"){
        if(data.password!==data.confirmPassword) throw new Error("Your passwords do not match.");
        if(data.password.length<8) throw new Error("Use a password with at least 8 characters.");
        if(!data.terms) throw new Error("Please accept the Terms and Privacy Policy to continue.");
        const {data:result,error}=await sb.auth.signUp({email:data.email.trim(),password:data.password,options:{data:{full_name:data.fullName.trim()},emailRedirectTo:location.origin+"/auth/login.html"}});
        if(error) throw error;
        showMessage(result.session ? "Your account is ready. You can now log in." : "Account created. Check your email for a verification link before logging in.","success");
        form.reset();
      } else if(mode==="login"){
        const {error}=await sb.auth.signInWithPassword({email:data.email.trim(),password:data.password});
        if(error) throw error;
        location.href="/dashboard/";
      } else if(mode==="reset"){
        const {error}=await sb.auth.resetPasswordForEmail(data.email.trim(),{redirectTo:location.origin+"/auth/update-password.html"});
        if(error) throw error;
        showMessage("If an account exists for that email, a password reset link has been sent.","success");
      } else if(mode==="update"){
        if(data.password!==data.confirmPassword) throw new Error("Your passwords do not match.");
        if(data.password.length<8) throw new Error("Use a password with at least 8 characters.");
        const {error}=await sb.auth.updateUser({password:data.password});
        if(error) throw error;
        showMessage("Password updated. You can now log in.","success");
      }
    } catch(error){showMessage(error.message||"Something went wrong. Please try again.");}
    finally{setBusy(false);}
  });
  if(form.dataset.authForm==="update"){
    getClientSafe().then(async sb=>{const {data}=await sb.auth.getSession();if(!data.session)showMessage("Open the password reset link from your email on this device.");});
  }
  function getClientSafe(){try{return Promise.resolve(getClient())}catch(e){return Promise.reject(e)}}
})();