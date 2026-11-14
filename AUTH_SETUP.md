# PillMate authentication setup

The authentication UI is added as a static frontend and uses Supabase Auth. It is not live until a Supabase project is configured.

## 1. Create/configure Supabase
- Create a Supabase project.
- In Project Settings → API, copy the Project URL and the anon/publishable key.
- Put those two public values in `auth/supabase-config.js`.
- Never put a Supabase `service_role` key in browser code or commit it to GitHub.

## 2. Configure Auth URLs
In Supabase Authentication → URL Configuration:
- Set Site URL to the production site origin (for example, https://thekiran.in).
- Add Redirect URLs for the production origin and local preview, including `https://thekiran.in/auth/login.html` and `https://thekiran.in/auth/update-password.html`.
- Enable email confirmations and configure SMTP before public launch. Supabase's default email service has sending limits.

## 3. Pages
- `/auth/login.html`
- `/auth/signup.html`
- `/auth/forgot-password.html`
- `/auth/update-password.html`

The landing-page navigation links to the login page. Registration is open to anyone. A successful login currently redirects to `/dashboard/`; the protected dashboard and user-specific medication tables are the next implementation step.

## 4. Important before launch
- Publish and review Terms and Privacy Policy pages before accepting real users.
- Add Row Level Security policies to every user-owned table before storing medication or personal information.
- Do not treat scheduled pill-count deductions as proof that a dose was swallowed; dose acknowledgement must be tracked separately.
